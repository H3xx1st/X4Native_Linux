// ===========================================================================
// x4llm — in-game LLM assistant (board computer) for X4: Foundations
//
// Phase 0: prove the async LLM round-trip.
//   * Lua raises "x4llm.ask" with a prompt string.
//   * A worker thread does a raw-socket HTTP POST to a llama.cpp server
//     (OpenAI-compatible /v1/chat/completions) WITHOUT blocking the game.
//   * The result is flushed on the main thread (on_frame_update) via
//     raise_lua_event("x4llm.response", text) — raise_lua_event may only be
//     called from a UI-thread event callback, never the worker.
//
// The worker-thread -> main-thread handoff mirrors the MD-event rule used by
// x4mp: capture the result, act (game/UI calls) on the main thread.
//
// Build: cmake (see CMakeLists.txt). Loads as an x4native extension (.so).
// ===========================================================================

#include "x4native_extension.h"
#include "x4_game_func_table.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <deque>
#include <condition_variable>
#include <chrono>
#include <nlohmann/json.hpp>

// Platform sockets: POSIX (Linux) or WinSock2 (Windows). The rest of the file
// is platform-independent and uses the sock_* macros below.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET xsock_t;
#define sock_close(fd)    closesocket(fd)
#define sock_last_err()   WSAGetLastError()
#define sock_send_flags() 0
#define SOCK_INVALID      INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int xsock_t;
#define sock_close(fd)    close(fd)
#define sock_last_err()   errno
#define sock_send_flags() MSG_NOSIGNAL   // avoid SIGPIPE killing the game on send
#define SOCK_INVALID      (-1)
#endif

static X4NativeAPI* g_api = nullptr;
static X4GameFunctions* g_game = nullptr;

// ---- LLM endpoint configuration -----------------------------------------
static std::string g_llm_url = "http://192.168.1.16:5001/v1"; // X4MP_LLM_URL
static std::string g_llm_model = "local";                     // X4MP_LLM_MODEL
static std::string g_llm_system =
    "You are the ship's onboard computer (board computer) for X4: Foundations. "
    "Be concise and helpful. Answer directly; use short sentences."; // X4MP_LLM_SYSTEM
static int g_llm_max_tokens = 512;                              // X4MP_LLM_MAXTOKENS
static double g_llm_temperature = 0.4;                          // X4MP_LLM_TEMPERATURE
// Additional sampling parameters (llama.cpp OpenAI-compatible API). Tunable
// live via x4llm_settings.txt (see load_settings) — no restart needed.
// Defaults mirror llama.cpp's own defaults.
static int g_llm_top_k = 40;                 // -1 disables
static double g_llm_top_p = 0.9;             // 1.0 disables
static double g_llm_min_p = 0.05;            // 0.0 disables
static double g_llm_top_n_sigma = 0.0;       // 0.0 disables
static double g_llm_presence_penalty = 0.0;  // 0.0 disables
static double g_llm_typical_p = 1.0;         // 1.0 disables
static double g_llm_repeat_penalty = 1.1;
static int g_llm_enable_thinking = 0;        // 0 disables reasoning (faster)

// ---- Async response state ------------------------------------------------
static std::atomic<bool> g_busy{false};
static std::mutex g_resp_mutex;
static std::string g_resp;
static std::atomic<bool> g_resp_ready{false};

// Set when the universe is fully built (framework forwards on_universe_ready).
// Game entity queries are only safe after this.
static std::atomic<bool> g_universe_ready{false};

// ---- Game-context snapshot caps -----------------------------------------
// Bounds how much live state we feed the model per question (keeps latency and
// attention in check even though the server has a huge context window). Ships
// are sampled and grouped by class; stations/sectors are listed with a cap.
static const size_t   CTX_SHIP_SAMPLE = 250;  // max ship ids examined for grouping
static const uint32_t CTX_STATION_CAP = 32;   // max station ids fetched
static const uint32_t CTX_SECTOR_CAP  = 24;   // max sector ids fetched
static const size_t   CTX_MAX_CHARS   = 9000; // hard truncation of the whole context
static const int      CTX_ENCY_TOPN   = 6;    // encyclopedia entries injected per query
static const size_t   CTX_ENCY_DESC   = 520;  // max chars of each injected description

// ---- Encyclopedia lore index -------------------------------------------
// Pre-extracted offline by tools/build_encyclopedia_index.py into
// extensions/x4llm/data/encyclopedia.index (category \x1f id \x1f name \x1f desc).
// Loaded once (lazily) and keyword-searched per question for relevant lore.
struct EncyEntry {
    std::string category;
    std::string name;
    std::string desc;
    std::string hay_name;  // lowercased, for retrieval
    std::string hay_desc;  // lowercased
};
static std::vector<EncyEntry> g_ency;
static std::atomic<bool> g_ency_loaded{false};

static std::string ctx_to_lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// ---- Small helpers -------------------------------------------------------
static void logi(const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_api && g_api->log) g_api->log(X4NATIVE_LOG_INFO, buf);
}

// Parse "http://host:port/base" into host/port/base.
static bool parse_llm_url(const char* url, std::string& host, int& port, std::string& base) {
    std::string u = url;
    size_t scheme = u.find("://");
    std::string rest = (scheme != std::string::npos) ? u.substr(scheme + 3) : u;
    size_t slash = rest.find('/');
    std::string hostport = (slash != std::string::npos) ? rest.substr(0, slash) : rest;
    base = (slash != std::string::npos) ? rest.substr(slash) : "/";
    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        host = hostport.substr(0, colon);
        port = atoi(hostport.substr(colon + 1).c_str());
    } else { host = hostport; port = 80; }
    return !host.empty() && port > 0 && port < 65536;
}

// Minimal JSON string escaping for embedding into a request body.
static std::string jesc(const std::string& s) {
    std::string o; o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
                else o += c;
        }
    }
    return o;
}

// Extract the assistant "content" from a /chat/completions response. If empty
// (some reasoning models put everything in reasoning_content), fall back to it.
static bool extract_content(const std::string& resp, std::string& out) {
    auto grab = [&](const char* field) -> bool {
        std::string needle = std::string("\"") + field + "\":\"";
        size_t p = resp.find(needle);
        if (p == std::string::npos) return false;
        p += needle.size();
        std::string s;
        while (p < resp.size()) {
            char c = resp[p];
            if (c == '\\') {
                if (p + 1 >= resp.size()) break;
                char e = resp[p + 1];
                switch (e) {
                    case 'n': s += '\n'; break;
                    case 't': s += '\t'; break;
                    case 'r': s += '\r'; break;
                    case '"': s += '"'; break;
                    case '\\': s += '\\'; break;
                    case '/': s += '/'; break;
                    case 'u':
                        if (p + 5 < resp.size()) {
                            char hex[5] = {resp[p+2], resp[p+3], resp[p+4], resp[p+5], 0};
                            int cp = (int)strtol(hex, nullptr, 16);
                            if (cp < 128) s += (char)cp;
                            else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
                            else { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
                            p += 5;
                        }
                        break;
                    default: s += e; break;
                }
                p += 2;
            } else if (c == '"') { break; }
            else { s += c; p++; }
        }
        out = s;
        return true;
    };
    if (grab("content") && !out.empty()) return true;
    return grab("reasoning_content"); // fallback
}

// Blocking HTTP POST (runs on the worker thread). Returns true on success.
static bool llm_http(const std::string& host, int port, const std::string& path,
                     const std::string& body, std::string& resp_out) {
#if defined(_WIN32)
    static WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { logi("x4llm: WSAStartup failed"); return false; }
#endif
    xsock_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == SOCK_INVALID) { logi("x4llm: socket() failed"); return false; }
    struct sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host.c_str(), &sa.sin_addr) != 1) { sock_close(fd); logi("x4llm: bad host '%s'", host.c_str()); return false; }

    // Connect (no explicit timeout: a big prompt can take a while to start).
    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        sock_close(fd);
        logi("x4llm: connect %s:%d failed (errno=%d)", host.c_str(), port, sock_last_err());
        return false;
    }

    char hdr[512];
    int hl = snprintf(hdr, sizeof(hdr),
        "POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n",
        path.c_str(), host.c_str(), body.size());
    std::string req(hdr, (size_t)hl);
    req += body;

    size_t sent = 0;
    bool send_ok = true;
    while (sent < req.size()) {
        int n = (int)send(fd, req.data() + sent, (int)(req.size() - sent), sock_send_flags());
        if (n <= 0) { send_ok = false; break; }
        sent += (size_t)n;
    }
    std::string resp;
    char buf[8192];
    int rn;
    while ((rn = (int)recv(fd, buf, sizeof(buf), 0)) > 0) resp.append(buf, (size_t)rn);
    sock_close(fd);
    resp_out = resp;
    return send_ok;
}

// Read x4llm_settings.txt (simple key=value) so parameters can be tuned LIVE:
// edit the file and the next request picks it up (no restart). Keys not present
// keep their current value (env var / built-in default). Path: X4MP_LLM_SETTINGS
// env var, else "extensions/x4llm/x4llm_settings.txt" relative to the game dir.
static void load_settings() {
    static char pathbuf[512] = "extensions/x4llm/x4llm_settings.txt";
    const char* e = std::getenv("X4MP_LLM_SETTINGS");
    const char* path = (e && *e) ? e : pathbuf;
    FILE* f = std::fopen(path, "r");
    if (!f) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            logi("x4llm: settings file not found at '%s' (using env vars / defaults; set X4MP_LLM_SETTINGS for an absolute path)", path);
        }
        return;
    }
    auto trim = [](std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        size_t b = s.find_last_not_of(" \t\r\n");
        s = (a == std::string::npos) ? std::string() : s.substr(a, b - a + 1);
    };
    char line[4096];
    bool found = false;
    while (std::fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\0') continue;
        char* eq = std::strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        std::string key = line, val = eq + 1;
        trim(key); trim(val);
        if (val.empty()) continue;
        if (key == "llm_url") { g_llm_url = val; found = true; }
        else if (key == "model") { g_llm_model = val; found = true; }
        else if (key == "temperature") { double v = atof(val.c_str()); if (v >= 0 && v <= 2) { g_llm_temperature = v; found = true; } }
        else if (key == "top_k") { g_llm_top_k = atoi(val.c_str()); found = true; }
        else if (key == "top_p") { double v = atof(val.c_str()); if (v >= 0 && v <= 1) { g_llm_top_p = v; found = true; } }
        else if (key == "min_p") { double v = atof(val.c_str()); if (v >= 0 && v <= 1) { g_llm_min_p = v; found = true; } }
        else if (key == "top_n_sigma") { g_llm_top_n_sigma = atof(val.c_str()); found = true; }
        else if (key == "presence_penalty") { g_llm_presence_penalty = atof(val.c_str()); found = true; }
        else if (key == "typical_p") { double v = atof(val.c_str()); if (v >= 0 && v <= 1) { g_llm_typical_p = v; found = true; } }
        else if (key == "repeat_penalty") { g_llm_repeat_penalty = atof(val.c_str()); found = true; }
        else if (key == "max_tokens") { int v = atoi(val.c_str()); if (v > 0) { g_llm_max_tokens = v; found = true; } }
        else if (key == "enable_thinking") { g_llm_enable_thinking = (atoi(val.c_str()) != 0); found = true; }
        else if (key == "system") { g_llm_system = val; found = true; }
    }
    std::fclose(f);
    if (found) logi("x4llm: settings loaded from %s (url=%s temp=%.3f top_k=%d top_p=%.3f min_p=%.3f top_n_sigma=%.3f think=%d)",
                    path, g_llm_url.c_str(), g_llm_temperature, g_llm_top_k, g_llm_top_p,
                    g_llm_min_p, g_llm_top_n_sigma, g_llm_enable_thinking);
}

// Load the encyclopedia index once (lazy). Runs on the UI thread; ~600KB read
// + parse is a few ms, done at most once per game session.
static void load_encyclopedia() {
    if (g_ency_loaded) return;
    g_ency_loaded = true;   // set once so a failure never retries every ask
    std::string path = "extensions/x4llm/data/encyclopedia.index";
    if (const char* e = std::getenv("X4LLM_ENCY_INDEX")) { if (*e) path = e; }
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) { logi("x4llm: encyclopedia index not found at '%s'", path.c_str()); return; }
    std::string all;
    char tmp[8192];
    size_t r;
    while ((r = std::fread(tmp, 1, sizeof(tmp), f)) > 0) all.append(tmp, r);
    std::fclose(f);

    size_t start = 0;
    while (start < all.size()) {
        size_t nl = all.find('\n', start);
        std::string rec = all.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (nl == std::string::npos) break;
        start = nl + 1;
        while (!rec.empty() && (rec.back() == '\r' || rec.back() == '\n')) rec.pop_back();
        if (rec.empty()) continue;
        // fields: 0=category, 1=id, 2=name, 3=desc (split on 0x1f)
        std::string fields[4];
        int nf = 0;
        while (nf < 4) {
            size_t p = rec.find('\x1f');
            if (p == std::string::npos) { fields[nf++] = rec; rec.clear(); }
            else { fields[nf++] = rec.substr(0, p); rec.erase(0, p + 1); }
        }
        if (fields[2].empty()) continue;   // name is required
        EncyEntry e;
        e.category = fields[0];
        e.name     = fields[2];
        e.desc     = fields[3];
        e.hay_name = ctx_to_lower(e.name);
        e.hay_desc = ctx_to_lower(e.desc);
        g_ency.push_back(std::move(e));
    }
    logi("x4llm: encyclopedia loaded %zu entries", g_ency.size());
}

// Keyword retrieval: rank encyclopedia entries against the question and return
// the top few as formatted context text (or "" if nothing matches).
static std::string search_encyclopedia(const std::string& query, int topn) {
    if (g_ency.empty() || topn <= 0) return "";
    // Tokenize: lowercase [a-z0-9]+ words, drop short/stop words.
    static const char* stops[] = {"the","a","an","and","or","of","to","in",
        "is","are","be","what","which","who","whom","how","why","when",
        "do","does","did","i","you","my","your","me","with","for","on",
        "about","there","this","that","it","its","as","at","by"};
    auto is_stop = [&](const std::string& w) {
        if ((int)w.size() < 3) return true;
        for (auto s : stops) if (w == s) return true;
        return false;
    };
    std::vector<std::string> terms;
    std::string cur;
    for (char ch : ctx_to_lower(query)) {
        if (isalnum((unsigned char)ch)) cur += ch;
        else { if (!cur.empty()) { if (!is_stop(cur)) terms.push_back(cur); cur.clear(); } }
    }
    if (!cur.empty() && !is_stop(cur)) terms.push_back(cur);
    if (terms.empty()) return "";

    std::vector<std::pair<int, size_t>> hits;   // {score, index}
    hits.reserve(64);
    for (size_t i = 0; i < g_ency.size(); ++i) {
        int score = 0;
        for (const auto& t : terms) {
            if (g_ency[i].hay_name == t) score += 6;                 // exact name match
            else if (g_ency[i].hay_name.find(t) != std::string::npos) score += 3;
            else if (g_ency[i].hay_desc.find(t) != std::string::npos) score += 1;
        }
        if (score > 0) hits.push_back({score, i});
    }
    if (hits.empty()) return "";
    std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) {
        return a.first > b.first || (a.first == b.first && a.second < b.second);
    });

    std::string out;
    for (int k = 0; k < topn && k < (int)hits.size(); ++k) {
        const EncyEntry& e = g_ency[hits[k].second];
        std::string body = e.desc;
        if (body.size() > CTX_ENCY_DESC) body = body.substr(0, CTX_ENCY_DESC) + "...";
        out += "  - ";
        if (!e.category.empty()) { out += "["; out += e.category; out += "] "; }
        out += e.name;
        if (!body.empty()) { out += ": "; out += body; }
        out += "\n";
    }
    return out;
}

// Build a bounded snapshot of the player's side (faction ships by class,
// stations, controlled sectors) as text. MUST run on the UI thread — game
// entity queries are not thread-safe. Returns "" when the universe isn't ready
// or the faction is unknown, so the caller falls back to a bare prompt.
//
// Follow-up (not yet enabled): recent news via GetMessages() — its category
// parameter semantics are unverified in this SDK, so it is deferred until we
// can confirm it is safe in-game (a UI-thread crash is unrecoverable).
static std::string build_context_snapshot(const std::string& question) {
    // Encyclopedia lore is pure data (no game API) -- always attempt it, fully
    // independent of whether the player faction can be resolved.
    load_encyclopedia();
    std::string ency = search_encyclopedia(question, CTX_ENCY_TOPN);

    // Live faction snapshot requires the universe ready AND a resolvable player
    // faction. Built into its own buffer so a failure here never suppresses the
    // encyclopedia section (the earlier bug that killed lore entirely).
    std::string live;
    bool have_live = false;
    if (!g_universe_ready) {
        logi("x4llm: live context skipped (universe not ready)");
    } else if (!g_game || !g_game->GetPlayerFactionName || !g_game->GetAllFactionShips) {
        logi("x4llm: live context skipped (game funcs null, game=%p)", (const void*)g_game);
    } else {
        // Resolve the player's faction id + display name. GetPlayerFactionName
        // is primary; observed to come back empty on some Linux runs, so fall
        // back to the player object's owner details, then to the vanilla "player"
        // id. Copy immediately -- returned pointers may be transient buffers.
        std::string fid, fname;
        if (g_game->GetPlayerFactionName) {
            if (const char* r = g_game->GetPlayerFactionName(true))  if (*r) fid = r;
            if (const char* n = g_game->GetPlayerFactionName(false)) if (*n) fname = n;
        }
        if (fid.empty() && g_game->GetPlayerObjectID && g_game->GetOwnerDetails2) {
            if (UniverseID po = g_game->GetPlayerObjectID()) {
                FactionDetails2 od = g_game->GetOwnerDetails2(po);
                if (od.factionID && *od.factionID) fid = od.factionID;
                if (fname.empty() && od.factionName && *od.factionName) fname = od.factionName;
            }
        }
        if (fid.empty()) fid = "player";
        if (fname.empty()) fname = fid;
        const char* rawid = fid.c_str();
        have_live = true;
        logi("x4llm: live context faction='%s' name='%s'", fid.c_str(), fname.c_str());
        live += "Player faction: ";
        live += fname;
        live += " ("; live += fid; live += ")\n";

            // Ships, grouped by class (top classes by count).
            if (g_game->GetNumAllFactionShips) {
                uint32_t total = g_game->GetNumAllFactionShips(rawid);
                uint32_t take  = (total < CTX_SHIP_SAMPLE) ? total : (uint32_t)CTX_SHIP_SAMPLE;
                if (take > 0) {
                    std::vector<UniverseID> ids(take);
                    uint32_t got = g_game->GetAllFactionShips(ids.data(), take, rawid);
                    std::map<std::string, int> byClass;
                    if (g_game->GetComponentClass)
                        for (uint32_t i = 0; i < got; ++i) {
                            const char* cls = g_game->GetComponentClass(ids[i]);
                            if (cls && *cls) byClass[cls]++;
                        }
                    std::vector<std::pair<const std::string*, int>> ranked;
                    ranked.reserve(byClass.size());
                    for (const auto& kv : byClass) ranked.emplace_back(&kv.first, kv.second);
                    std::sort(ranked.begin(), ranked.end(),
                              [](const auto& a, const auto& b){ return a.second > b.second; });
                    live += "Ships ("; live += std::to_string(total);
                    if (total > (uint32_t)CTX_SHIP_SAMPLE) { live += " total, sampled "; live += std::to_string(got); }
                    live += "):\n";
                    for (size_t i = 0; i < ranked.size() && i < 12; ++i) {
                        live += "  - "; live += *ranked[i].first; live += " x"; live += std::to_string(ranked[i].second); live += "\n";
                    }
                    if ((int)total > (int)got) { live += "  +"; live += std::to_string(total - got); live += " more ships\n"; }
                }
            }

            // Stations.
            if (g_game->GetAllFactionStations && g_game->GetComponentName) {
                std::vector<UniverseID> ids(CTX_STATION_CAP);
                uint32_t got = g_game->GetAllFactionStations(ids.data(), CTX_STATION_CAP, rawid);
                if (got > 0) {
                    live += "Stations ("; live += std::to_string(got); "):\n";
                    for (uint32_t i = 0; i < got && i < 12; ++i) {
                        const char* nm = g_game->GetComponentName(ids[i]);
                        if (nm && *nm) { live += "  - "; live += nm; live += "\n"; }
                    }
                    if (got > 12) { live += "  +"; live += std::to_string(got - 12); live += " more\n"; }
                }
            }

            // Sectors under the player's control.
            if (g_game->GetSectorsByOwner && g_game->GetComponentName) {
                std::vector<UniverseID> ids(CTX_SECTOR_CAP);
                uint32_t got = g_game->GetSectorsByOwner(ids.data(), CTX_SECTOR_CAP, rawid);
                if (got > 0) {
                    live += "Controlled sectors ("; live += std::to_string(got); "):\n";
                    for (uint32_t i = 0; i < got && i < 12; ++i) {
                        const char* nm = g_game->GetComponentName(ids[i]);
                        if (nm && *nm) { live += "  - "; live += nm; live += "\n"; }
                    }
                    if (got > 12) { live += "  +"; live += std::to_string(got - 12); live += " more\n"; }
                }
            }
        }

    if (!have_live && ency.empty()) return "";

    std::string out;
    out += "[GAME CONTEXT: auto-appended reference information; treat the listed facts as true]\n";
    if (have_live) out += "--- Player's faction (live) ---\n" + live;
    if (!ency.empty()) out += "--- Relevant encyclopedia entries ---\n" + ency;
    out += "[/GAME CONTEXT]\n";

    if (out.size() > CTX_MAX_CHARS) {
        size_t cut = out.rfind('\n', CTX_MAX_CHARS);
        out = (cut != std::string::npos) ? out.substr(0, cut) : out.substr(0, CTX_MAX_CHARS);
        out += "  ...(context truncated)...\n[/GAME CONTEXT]\n";
    }
    return out;
}

// ---- Tool-calling --------------------------------------------------------
// The model may request read-only queries of the LIVE game via tool calls.
// Game entity APIs are not thread-safe, so execution MUST happen on the UI
// thread. The model runs on a worker thread, therefore the worker enqueues a
// ToolQuery and blocks on g_qcv; on_frame_update (UI thread) executes the
// queued queries and posts their results. Producer/consumer over g_queries.
struct ToolQuery {
    int id = 0;
    std::string name;
    nlohmann::json args;
    std::string result;
    bool done = false;
};
static std::deque<ToolQuery> g_queries;
static std::mutex g_qm;
static std::condition_variable g_qcv;
static std::atomic<int> g_qseq{1};
static const int TOOL_MAX_ROUNDS = 8;             // max tool-call rounds per question
static const int TOOL_QUERY_TIMEOUT_MS = 90000;   // give up waiting on the UI thread

static const char* TOOL_GUIDANCE =
    " You have tools to query the player's LIVE game state. Do NOT guess game "
    "facts -- call the right tool first. Use: player_ship for the ship you are "
    "currently in; my_ships, my_stations, my_sectors for your fleet, stations "
    "and territory; ship_status for a ship's hull/shield, consumables, equipment "
    "loadout and current orders/tasks; station_build_plan for what a station's "
    "build queue needs (including reserved builders); lookup_entry for in-game "
    "encyclopedia lore. After gathering data, give a "
    "concise final answer.";

// Resolve the player's faction id (same fallback chain as the snapshot).
static std::string x4llm_faction_id() {
    auto* g = g_game;
    if (!g) return "player";
    std::string fid;
    if (g->GetPlayerFactionName) { if (const char* r = g->GetPlayerFactionName(true)) if (*r) fid = r; }
    if (fid.empty() && g->GetPlayerObjectID && g->GetOwnerDetails2) {
        if (UniverseID po = g->GetPlayerObjectID()) {
            FactionDetails2 od = g->GetOwnerDetails2(po);
            if (od.factionID && *od.factionID) fid = od.factionID;
        }
    }
    if (fid.empty()) fid = "player";
    return fid;
}

static std::string tool_player_ship() {
    auto* g = g_game;
    if (!g || !g->GetPlayerObjectID) return "{\"error\":\"game api unavailable\"}";
    UniverseID p = g->GetPlayerObjectID();
    if (!p) return "{\"ship\":null,\"note\":\"the player is not currently piloting a ship (docked or at a station)\"}";
    nlohmann::json j; j["id"] = (uint64_t)p;
    if (g->GetComponentName)  { const char* n = g->GetComponentName(p);  if (n && *n) j["name"] = n; }
    if (g->GetComponentClass) { const char* c = g->GetComponentClass(p); if (c && *c) j["class"] = c; }
    if (g->GetOwnerDetails2)  { FactionDetails2 od = g->GetOwnerDetails2(p); if (od.factionID && *od.factionID) j["owner"] = od.factionID; }
    return j.dump();
}

static std::string tool_my_ships(const nlohmann::json& a) {
    auto* g = g_game;
    if (!g || !g->GetAllFactionShips) return "{\"error\":\"unavailable\"}";
    std::string fid = x4llm_faction_id();
    int limit = a.value("limit", 250);
    uint32_t total = g->GetNumAllFactionShips ? g->GetNumAllFactionShips(fid.c_str()) : 0;
    uint32_t take  = (total < (uint32_t)limit) ? total : (uint32_t)limit;
    nlohmann::json byclass = nlohmann::json::object();
    nlohmann::json named   = nlohmann::json::array();
    if (take > 0) {
        std::vector<UniverseID> ids(take);
        uint32_t got = g->GetAllFactionShips(ids.data(), take, fid.c_str());
        for (uint32_t i = 0; i < got; ++i) {
            const char* cls = g->GetComponentClass ? g->GetComponentClass(ids[i]) : nullptr;
            if (cls && *cls) byclass[cls] = byclass.value(cls, 0) + 1;
            if ((int)named.size() < 15 && g->GetComponentName) {
                const char* nm = g->GetComponentName(ids[i]);
                if (nm && *nm) named.push_back(nm);
            }
        }
    }
    nlohmann::json j; j["faction"] = fid; j["total"] = total; j["by_class"] = byclass; j["sample_names"] = named;
    return j.dump();
}

static std::string tool_my_stations(const nlohmann::json& a) {
    auto* g = g_game;
    if (!g || !g->GetAllFactionStations) return "{\"error\":\"unavailable\"}";
    std::string fid = x4llm_faction_id();
    int limit = a.value("limit", 32);
    std::vector<UniverseID> ids((size_t)limit);
    uint32_t got = g->GetAllFactionStations(ids.data(), (uint32_t)limit, fid.c_str());
    nlohmann::json arr = nlohmann::json::array();
    for (uint32_t i = 0; i < got; ++i) {
        nlohmann::json s;
        if (g->GetComponentName)  { const char* n = g->GetComponentName(ids[i]);  if (n && *n) s["name"] = n; }
        if (g->GetComponentClass) { const char* c = g->GetComponentClass(ids[i]); if (c && *c) s["class"] = c; }
        arr.push_back(s);
    }
    nlohmann::json j; j["faction"] = fid; j["count"] = got; j["stations"] = arr;
    return j.dump();
}

static std::string tool_my_sectors() {
    auto* g = g_game;
    if (!g || !g->GetSectorsByOwner) return "{\"error\":\"unavailable\"}";
    std::string fid = x4llm_faction_id();
    std::vector<UniverseID> ids(64);
    uint32_t got = g->GetSectorsByOwner(ids.data(), (uint32_t)ids.size(), fid.c_str());
    nlohmann::json arr = nlohmann::json::array();
    for (uint32_t i = 0; i < got; ++i) {
        if (g->GetComponentName) { const char* n = g->GetComponentName(ids[i]); if (n && *n) arr.push_back(n); }
    }
    nlohmann::json j; j["faction"] = fid; j["count"] = got; j["sectors"] = arr;
    return j.dump();
}

// Find a player station id by (case-insensitive) name substring; '' -> first.
static UniverseID find_station(const std::string& want) {
    auto* g = g_game;
    if (!g || !g->GetAllFactionStations) return 0;
    std::string fid = x4llm_faction_id();
    std::vector<UniverseID> ids(128);
    uint32_t got = g->GetAllFactionStations(ids.data(), (uint32_t)ids.size(), fid.c_str());
    for (uint32_t i = 0; i < got; ++i) {
        const char* nm = g->GetComponentName ? g->GetComponentName(ids[i]) : nullptr;
        if (!nm) continue;
        if (want.empty()) return ids[i];
        if (ctx_to_lower(nm).find(ctx_to_lower(want)) != std::string::npos) return ids[i];
    }
    return 0;
}

static std::string tool_station_build_plan(const nlohmann::json& a) {
    auto* g = g_game;
    if (!g) return "{\"error\":\"unavailable\"}";
    std::string want = a.value("station", "");
    UniverseID sid = find_station(want);
    if (!sid) return "{\"error\":\"no matching station\",\"hint\":\"call my_stations to list station names\"}";
    nlohmann::json j;
    if (g->GetComponentName) { const char* n = g->GetComponentName(sid); if (n && *n) j["station"] = n; }
    nlohmann::json totals = nlohmann::json::object();  // ware -> total amount
    auto add_ware = [&](const char* ware, int amt){ if (ware && *ware) totals[ware] = totals.value(ware, 0) + amt; };
    // 1) PLANNED modules (queued, not yet built) -- no holomap required.
    //    Signature: GetPlannedStationModules(result, resultlen, defensibleid, includeall).
    nlohmann::json planned = nlohmann::json::array();
    int plan_count = 0;
    if (g->GetNumPlannedStationModules && g->GetPlannedStationModules) {
        size_t pn = g->GetNumPlannedStationModules(sid, true);
        if (pn > 0) {
            std::vector<UIConstructionPlanEntry> plan(pn < 256 ? pn : 256);
            size_t got = g->GetPlannedStationModules(plan.data(), (uint32_t)plan.size(), sid, true);
            plan_count = (int)got;
            for (size_t i = 0; i < got && i < 60; ++i) {
                nlohmann::json e;
                if (plan[i].macroid) e["macro"] = plan[i].macroid;
                planned.push_back(e);
            }
        }
    }
    j["planned_modules"] = planned;
    j["planned_module_count"] = plan_count;

    // 2) Build queue: builders + per-module tasks.
    //    Builders and build tasks live on the station's BUILDSTORAGE (component
    //    "buildstorage"), NOT the station container — a bare sid is why
    //    GetAssignedConstructionVessels / GetBuildModules failed before.
    nlohmann::json tasks = nlohmann::json::array();
    std::vector<std::string> builder_names;
    auto add_builder = [&](const char* n){
        if (!n || !*n) return;
        for (auto& s : builder_names) if (s == n) return;
        builder_names.emplace_back(n);
    };
    UniverseID bstorage = g->GetContextByClass ? g->GetContextByClass(sid, "buildstorage", false) : 0;
    // Match the UI's exact buildstorage source: GetComponentData(station, "buildstorage").
    UniverseID bstorage_ui = 0;
    if (g_api && g_api->get_lua_property_str) {
        char sidstr[32]; snprintf(sidstr, sizeof sidstr, "%llu", (unsigned long long)sid);
        X4nLuaKey key; key.type = X4N_KEY_STRING; key.v.s = sidstr;
        char bs[32] = {0};
        if (g_api->get_lua_property_str("GetComponentData", key, "buildstorage", bs, sizeof(bs)) && bs[0])
            bstorage_ui = (UniverseID)strtoull(bs, nullptr, 10);
    }
    std::vector<UniverseID> bcands;
    auto push_cand = [&](UniverseID c){ for (auto& x : bcands) if (x==c) return; if (c) bcands.push_back(c); };
    push_cand(bstorage); push_cand(bstorage_ui); push_cand(sid);
    std::string cand_log;
    for (UniverseID c : bcands) { char b[32]; snprintf(b, sizeof b, "%llu ", (unsigned long long)c); cand_log += b; }

    // 2a) Assigned builders (across every plausible container).
    int total_assigned = 0;
    if (g->GetNumAssignedConstructionVessels && g->GetAssignedConstructionVessels) {
        for (UniverseID c : bcands) {
            uint32_t nb = g->GetNumAssignedConstructionVessels(c);
            total_assigned += nb;
            if (nb == 0) continue;
            std::vector<UniverseID> bv(nb < 32 ? nb : 32);
            uint32_t got = g->GetAssignedConstructionVessels(bv.data(), (uint32_t)bv.size(), c);
            for (uint32_t i = 0; i < got; ++i)
                if (g->GetComponentName) add_builder(g->GetComponentName(bv[i]));
        }
    }
    logi("station_build_plan buildstorage candidates=[%s] total_assigned=%d", cand_log.c_str(), total_assigned);

    // 2b) Build modules -> build tasks (the confirmed construction sequence).
    if (g->GetNumBuildModules && g->GetBuildModules && g->GetNumBuildTasks && g->GetBuildTasks) {
        for (UniverseID c : bcands) {
            if (!c) continue;
            uint32_t nbm = g->GetNumBuildModules(c);
            if (nbm == 0) continue;
            std::vector<UniverseID> bms(nbm < 32 ? nbm : 32);
            uint32_t gotbm = g->GetBuildModules(bms.data(), (uint32_t)bms.size(), c);
            for (uint32_t b = 0; b < gotbm; ++b) {
                uint32_t nt = g->GetNumBuildTasks(c, bms[b], true, true);
                if (nt == 0) continue;
                std::vector<BuildTaskInfo> bt(nt < 80 ? nt : 80);
                uint32_t actual = g->GetBuildTasks(bt.data(), (uint32_t)bt.size(), c, bms[b], true, true);
                for (uint32_t i = 0; i < actual; ++i) {
                    nlohmann::json t;
                    if (bt[i].macro) t["module"] = bt[i].macro;
                    t["missing_resources"] = bt[i].ismissingresources;
                    t["queue_position"] = bt[i].queueposition;
                    t["price"] = bt[i].price;
                    if (bt[i].buildercomponent && g->GetComponentName) add_builder(g->GetComponentName(bt[i].buildercomponent));
                    tasks.push_back(t);
                }
            }
            if (!tasks.empty()) break;
        }
    }
    j["build_tasks"] = tasks;
    nlohmann::json builders = nlohmann::json::array();
    for (auto& s : builder_names) builders.push_back(s);
    j["builders"] = builders;

    // 2c) Reserved / incoming builders. There is NO station-side API for a CV that is
    //     assigned but not yet docked (GetAssignedConstructionVessels above only counts
    //     docked ones). The reliable signal is ship-side: a player ship whose order queue
    //     contains a "DeployToStation" (construction deploy) order is a reserved builder.
    //     List them so the answer can name the Walrus even while it's still inbound.
    nlohmann::json reserved = nlohmann::json::array();
    if (g->GetAllFactionShips && g->GetNumOrders && g->GetOrders3) {
        std::string fid = x4llm_faction_id();
        uint32_t nships = g->GetNumAllFactionShips ? g->GetNumAllFactionShips(fid.c_str()) : 0;
        if (nships) {
            std::vector<UniverseID> ships(nships < 64 ? nships : 64);
            uint32_t gs = g->GetAllFactionShips(ships.data(), (uint32_t)ships.size(), fid.c_str());
            for (uint32_t i = 0; i < gs; ++i) {
                uint32_t no = g->GetNumOrders(ships[i]);
                if (!no) continue;
                std::vector<Order3> ob(no < 16 ? no : 16);
                uint32_t go = g->GetOrders3(ob.data(), (uint32_t)ob.size(), ships[i]);
                bool deploy = false;
                for (uint32_t k = 0; k < go; ++k)
                    if (ob[k].orderdef && strstr(ob[k].orderdef, "DeployToStation")) { deploy = true; break; }
                if (deploy && g->GetComponentName) {
                    const char* nm = g->GetComponentName(ships[i]);
                    if (nm && *nm && (int)reserved.size() < 20) reserved.push_back(nm);
                }
            }
        }
    }
    if (!reserved.empty()) j["reserved_builders"] = reserved;
    if (total_assigned == 0 && reserved.empty())
        j["builder_note"] = "no builder is docked or has a construction order right now; construction stays paused until a vessel deploys here";

    // 3) Required build wares. Prefer the confirmed build sequence (the modules
    //    actually being built = the "changes"); fall back to all planned modules.
    //    Recipe chain (engine ware DB, valid out-of-UI incl. Boron/Terran):
    //      macro -> ware (GetMacroData) -> method (GetWareData productionmethods)
    //            -> recipe (GetProductionMethodResources, typed).
    std::vector<std::string> res_macros;
    for (auto& t : tasks) { std::string m = t.value("module", std::string()); if (!m.empty()) res_macros.push_back(m); }
    if (res_macros.empty())
        for (auto& p : planned) { std::string m = p.value("macro", std::string()); if (!m.empty()) res_macros.push_back(m); }
    if (g->GetNumProductionMethodResources && g->GetProductionMethodResources &&
        g_api && g_api->get_lua_property_str) {
        for (auto& m : res_macros) {
            char target[256] = {0};
            X4nLuaKey k{}; k.type = X4N_KEY_STRING; k.v.s = m.c_str();
            if (!g_api->get_lua_property_str("X4LLM_build_target", k, "", target, sizeof(target)))
                continue;
            std::string t(target);
            auto bar = t.find('|');
            if (bar == std::string::npos) continue;
            std::string ware = t.substr(0, bar);
            std::string method = t.substr(bar + 1);
            if (ware.empty() || method.empty()) continue;
            uint32_t n = g->GetNumProductionMethodResources(ware.c_str(), method.c_str());
            if (n == 0) continue;
            std::vector<UIWareAmount> res(n < 64 ? n : 64);
            uint32_t got = g->GetProductionMethodResources(res.data(), (uint32_t)res.size(),
                                                           ware.c_str(), method.c_str());
            for (uint32_t w = 0; w < got; ++w)
                if (res[w].wareid) add_ware(res[w].wareid, (int)res[w].amount);
        }
    }

    // 4) Existing (built) modules, for context.
    nlohmann::json modules = nlohmann::json::array();
    if (g->GetNumStationModules && g->GetStationModules) {
        uint32_t nm = g->GetNumStationModules(sid, true, false);
        uint32_t cap = nm < 80 ? nm : 80;
        if (cap > 0) {
            std::vector<UniverseID> mods(cap);
            uint32_t gm = g->GetStationModules(mods.data(), cap, sid, true, false);
            for (uint32_t i = 0; i < gm; ++i)
                if (g->GetComponentClass) { const char* c = g->GetComponentClass(mods[i]); if (c && *c) modules.push_back(c); }
        }
    }
    j["existing_modules"] = modules;
    j["total_required_wares"] = totals;
    return j.dump();
}

// ---- Ship status / loadout / orders helpers ------------------------------
// Human-readable name for a macro id (bare-global GetMacroData via the bridge).
static std::string macro_name(const char* macro) {
    if (!macro || !*macro) return std::string();
    if (!g_api || !g_api->get_lua_property_str) return macro;
    char buf[128] = {0};
    X4nLuaKey k{}; k.type = X4N_KEY_STRING; k.v.s = macro;
    if (g_api->get_lua_property_str("GetMacroData", k, "name", buf, sizeof buf) && buf[0])
        return buf;
    return macro;  // fall back to the raw macro id
}

// Aggregate one AmmoData-backed consumable category (missiles, mines, ...).
static void ammo_add(nlohmann::json& obj, const char* key,
                     uint32_t (*numfn)(UniverseID),
                     uint32_t (*getfn)(AmmoData*, uint32_t, UniverseID),
                     UniverseID ship) {
    uint32_t n = numfn(ship);
    if (!n) return;
    std::vector<AmmoData> b(n < 24 ? n : 24);
    uint32_t got = getfn(b.data(), (uint32_t)b.size(), ship);
    int total = 0; uint32_t cap = 0;
    nlohmann::json types = nlohmann::json::array();
    for (uint32_t i = 0; i < got; ++i) {
        total += b[i].amount;
        if (b[i].capacity > cap) cap = b[i].capacity;
        if ((int)types.size() < 6 && b[i].macro)
            types.push_back(macro_name(b[i].macro) + " x" + std::to_string(b[i].amount));
    }
    if (total == 0 && cap == 0) return;
    obj[key] = {{"count", total}, {"capacity", (int)cap}, {"types", types}};
}

// Find a player-faction ship: numeric arg -> id, '' -> current ship, else name match.
static UniverseID find_ship(const std::string& want) {
    auto* g = g_game;
    if (!g) return 0;
    if (!want.empty() && want.find_first_not_of("0123456789") == std::string::npos)
        return (UniverseID)strtoull(want.c_str(), nullptr, 10);
    if (want.empty() && g->GetPlayerObjectID) {
        UniverseID p = g->GetPlayerObjectID();
        if (p) return p;
    }
    if (!g->GetAllFactionShips) return 0;
    std::string fid = x4llm_faction_id();
    uint32_t total = g->GetNumAllFactionShips ? g->GetNumAllFactionShips(fid.c_str()) : 0;
    uint32_t take = total < 256 ? total : 256;
    if (take == 0) return 0;
    std::vector<UniverseID> ids(take);
    uint32_t got = g->GetAllFactionShips(ids.data(), take, fid.c_str());
    for (uint32_t i = 0; i < got; ++i) {
        const char* nm = g->GetComponentName ? g->GetComponentName(ids[i]) : nullptr;
        if (!nm) continue;
        if (want.empty()) return ids[i];
        if (ctx_to_lower(nm).find(ctx_to_lower(want)) != std::string::npos) return ids[i];
    }
    return 0;
}

// Full read-only snapshot of a ship: hull/shield status, consumables, orders,
// loadout (weapons/turrets/shields/engines/thrusters). Answers 'what is the Ray
// carrying / equipped / doing / how damaged is it' questions.
static std::string tool_ship_status(const nlohmann::json& a) {
    auto* g = g_game;
    if (!g) return "{\"error\":\"unavailable\"}";
    std::string want = a.value("ship", "");
    UniverseID ship = find_ship(want);
    if (!ship) return "{\"error\":\"no matching ship\",\"hint\":\"call my_ships to list ship names, or pass a numeric ship id\"}";

    nlohmann::json j;
    j["id"] = (uint64_t)ship;
    if (g->GetComponentName)  { const char* n = g->GetComponentName(ship);  if (n && *n) j["name"] = n; }
    if (g->GetComponentClass) { const char* c = g->GetComponentClass(ship); if (c && *c) j["class"] = c; }
    if (g->GetOwnerDetails2)  { FactionDetails2 od = g->GetOwnerDetails2(ship); if (od.factionID && *od.factionID) j["owner"] = od.factionID; }

    // ---- hull / shield --------------------------------------------------
    nlohmann::json hsh = nlohmann::json::object();
    if (g->GetComponentDetails) {
        ComponentDetails d = g->GetComponentDetails(ship, "");
        hsh["hull_percent"] = (int)(d.hull + 0.5f);
        hsh["shield_percent"] = (int)(d.shield + 0.5f);
        hsh["has_shield"] = d.hasShield;
        hsh["speed"] = d.speed;
    }
    // MJ values via the bare-global GetComponentData bridge.
    if (g_api && g_api->get_lua_property_str) {
        char sidstr[32]; snprintf(sidstr, sizeof sidstr, "%llu", (unsigned long long)ship);
        X4nLuaKey k{}; k.type = X4N_KEY_STRING; k.v.s = sidstr;
        char v[64] = {0};
        if (g_api->get_lua_property_str("GetComponentData", k, "hull", v, sizeof v) && v[0]) hsh["hull_mj"] = atoi(v);
        if (g_api->get_lua_property_str("GetComponentData", k, "hullmax", v, sizeof v) && v[0]) hsh["hull_max_mj"] = atoi(v);
        if (g_api->get_lua_property_str("GetComponentData", k, "shield", v, sizeof v) && v[0]) hsh["shield_mj"] = atoi(v);
        if (g_api->get_lua_property_str("GetComponentData", k, "shieldmax", v, sizeof v) && v[0]) hsh["shield_max_mj"] = atoi(v);
    }
    if (!hsh.empty()) j["hull_shield"] = hsh;

    // ---- consumables / ammo / deployables -------------------------------
    nlohmann::json ammo = nlohmann::json::object();
    if (g->GetNumAllMissiles && g->GetAllMissiles)
        ammo_add(ammo, "missiles", g->GetNumAllMissiles, g->GetAllMissiles, ship);
    if (g->GetNumAllMines && g->GetAllMines)
        ammo_add(ammo, "mines", g->GetNumAllMines, g->GetAllMines, ship);
    if (g->GetNumAllCountermeasures && g->GetAllCountermeasures)
        ammo_add(ammo, "countermeasures", g->GetNumAllCountermeasures, g->GetAllCountermeasures, ship);
    if (g->GetNumAllSatellites && g->GetAllSatellites)
        ammo_add(ammo, "satellites", g->GetNumAllSatellites, g->GetAllSatellites, ship);
    if (g->GetNumAllNavBeacons && g->GetAllNavBeacons)
        ammo_add(ammo, "nav_beacons", g->GetNumAllNavBeacons, g->GetAllNavBeacons, ship);
    if (g->GetNumAllResourceProbes && g->GetAllResourceProbes)
        ammo_add(ammo, "resource_probes", g->GetNumAllResourceProbes, g->GetAllResourceProbes, ship);
    if (g->GetNumAllLaserTowers && g->GetAllLaserTowers)
        ammo_add(ammo, "laser_towers", g->GetNumAllLaserTowers, g->GetAllLaserTowers, ship);
    if (g->GetNumAllUnits && g->GetAllUnits) {
        uint32_t n = g->GetNumAllUnits(ship, false);
        if (n) {
            std::vector<UnitData> b(n < 24 ? n : 24);
            uint32_t got = g->GetAllUnits(b.data(), (uint32_t)b.size(), ship, false);
            int total = 0; nlohmann::json types = nlohmann::json::array();
            for (uint32_t i = 0; i < got; ++i) {
                total += b[i].amount;
                if ((int)types.size() < 6 && b[i].macro) types.push_back(macro_name(b[i].macro) + " x" + std::to_string(b[i].amount));
            }
            if (total) ammo["drones"] = {{"count", total}, {"types", types}};
        }
    }
    if (!ammo.empty()) j["consumables"] = ammo;

    // ---- orders ---------------------------------------------------------
    nlohmann::json orders = nlohmann::json::object();
    int queued = 0;
    if (g->GetNumOrders && g->GetOrders3) {
        uint32_t n = g->GetNumOrders(ship);
        if (n) {
            std::vector<Order3> b(n < 32 ? n : 32);
            uint32_t got = g->GetOrders3(b.data(), (uint32_t)b.size(), ship);
            size_t cur = g->GetOrderQueueCurrentIdx ? g->GetOrderQueueCurrentIdx(ship) : 0;
            nlohmann::json queue = nlohmann::json::array();
            nlohmann::json current;
            for (uint32_t i = 0; i < got; ++i) {
                if (!b[i].enabled) continue;
                std::string nm = b[i].orderdef ? b[i].orderdef : "";
                if (g->GetOrderDefinition && !nm.empty()) {
                    OrderDefinition def;
                    if (g->GetOrderDefinition(&def, nm.c_str()) && def.name && *def.name) nm = def.name;
                }
                nlohmann::json q; q["order"] = nm;
                if (b[i].statename && *b[i].statename) q["state"] = b[i].statename;
                else if (b[i].state && *b[i].state) q["state"] = b[i].state;
                q["queue_index"] = (int)b[i].queueidx;
                queue.push_back(q); queued++;
                if (cur && b[i].queueidx == (size_t)cur) current = q;
            }
            orders["queue"] = queue;
            if (cur) orders["current_index"] = (int)cur;
            if (!current.is_null()) orders["current"] = current;
        }
    }
    // Default behaviour — applies when the order queue is EMPTY. Common standing
    // tasks (e.g. "Protect Station") live here, NOT in the queue (GetDefaultOrder).
    if (g->GetDefaultOrder) {
        Order d;
        if (g->GetDefaultOrder(&d, ship) && d.orderdef && *d.orderdef) {
            std::string nm = d.orderdef;
            if (g->GetOrderDefinition) {
                OrderDefinition def;
                if (g->GetOrderDefinition(&def, nm.c_str()) && def.name && *def.name) nm = def.name;
            }
            nlohmann::json db; db["order"] = nm;
            if (d.statename && *d.statename) db["state"] = d.statename;
            else if (d.state && *d.state) db["state"] = d.state;
            orders["default_behavior"] = db;
            if (queued == 0) orders["current"] = db;  // nothing queued -> default IS the active task
        }
    }
    if (queued == 0 && orders.find("default_behavior") == orders.end())
        orders["note"] = "no queued orders and no default behaviour set";
    j["orders"] = orders;

    // ---- loadout --------------------------------------------------------
    // Report an accurate per-component breakdown (name -> count). Grouped
    // components (turrets/shields/engines) carry their own count in
    // GetUpgradeGroupInfo2; individual ones (weapons) fall back to slot macros.
    nlohmann::json loadout = nlohmann::json::object();
    auto add_loadout = [&](const char* key, const char* type) {
        nlohmann::json items = nlohmann::json::object();
        auto bump = [&](const char* macro, int n) {
            if (!macro || !*macro || n <= 0) return;
            std::string nm = macro_name(macro);
            items[nm] = items.value(nm, 0) + n;
        };
        int total = 0;
        if (g->GetNumUpgradeGroups && g->GetUpgradeGroups2 && g->GetUpgradeGroupInfo2) {
            size_t ng = g->GetNumUpgradeGroups(ship, "");
            if (ng) {
                std::vector<UpgradeGroup2> gs(ng < 32 ? ng : 32);
                uint32_t got = g->GetUpgradeGroups2(gs.data(), (uint32_t)gs.size(), ship, "");
                for (uint32_t i = 0; i < got; ++i) {
                    UpgradeGroupInfo info = g->GetUpgradeGroupInfo2(ship, "", gs[i].contextid, gs[i].path, gs[i].group, type);
                    if (info.count > 0) { bump(info.currentmacro, (int)info.count); total += info.count; }
                }
            }
        }
        if (total == 0 && g->GetNumUpgradeSlots && g->GetUpgradeSlotCurrentMacro) {
            size_t n = g->GetNumUpgradeSlots(ship, "", type);
            for (size_t s = 1; s <= n && s <= 32; ++s) {
                const char* m = g->GetUpgradeSlotCurrentMacro(ship, 0, type, s);
                if (m && *m) { bump(m, 1); total++; }
            }
        }
        if (total > 0) loadout[key] = nlohmann::json{{"count", total}, {"items", items}};
    };
    add_loadout("weapons", "weapon");
    add_loadout("turrets", "turret");
    add_loadout("shields", "shield");
    add_loadout("engines", "engine");
    if (g->GetNumVirtualUpgradeSlots && g->GetVirtualUpgradeSlotCurrentMacro) {
        size_t nv = g->GetNumVirtualUpgradeSlots(ship, "", "thruster");
        nlohmann::json items = nlohmann::json::object();
        int thr = 0;
        for (size_t s = 1; s <= nv && s <= 32; ++s) {
            const char* m = g->GetVirtualUpgradeSlotCurrentMacro(ship, "thruster", s);
            if (m && *m) { std::string nm = macro_name(m); items[nm] = items.value(nm, 0) + 1; thr++; }
        }
        if (thr) loadout["thrusters"] = nlohmann::json{{"count", thr}, {"items", items}};
    }
    if (!loadout.empty()) j["loadout"] = loadout;

    return j.dump();
}

static std::string tool_lookup_entry(const nlohmann::json& a) {
    load_encyclopedia();
    std::string q = a.value("query", "");
    int n = a.value("count", 6);
    std::string res = search_encyclopedia(q, n);
    if (res.empty()) return nlohmann::json({{"error","no encyclopedia entries match"},{"query",q}}).dump();
    return nlohmann::json({{"query",q},{"entries",res}}).dump();
}

static std::string dispatch_tool(const std::string& name, const nlohmann::json& args) {
    if (name == "player_ship")        return tool_player_ship();
    if (name == "my_ships")           return tool_my_ships(args);
    if (name == "my_stations")        return tool_my_stations(args);
    if (name == "my_sectors")         return tool_my_sectors();
    if (name == "station_build_plan") return tool_station_build_plan(args);
    if (name == "ship_status")        return tool_ship_status(args);
    if (name == "lookup_entry")       return tool_lookup_entry(args);
    return "{\"error\":\"unknown tool\"}";
}

static const char* TOOLS_JSON = R"([
 {"type":"function","function":{"name":"player_ship","description":"Get the ship the player is currently piloting (name, class, owner). Returns ship=null if not in a ship.","parameters":{"type":"object","properties":{}}}},
 {"type":"function","function":{"name":"my_ships","description":"The player faction's ships grouped by class with counts, plus sample ship names.","parameters":{"type":"object","properties":{"limit":{"type":"integer","description":"max ships to examine, default 250"}}}}},
 {"type":"function","function":{"name":"my_stations","description":"The player faction's stations (name and class).","parameters":{"type":"object","properties":{"limit":{"type":"integer"}}}}},
 {"type":"function","function":{"name":"my_sectors","description":"Sectors under the player faction's control.","parameters":{"type":"object","properties":{}}}},
 {"type":"function","function":{"name":"ship_status","description":"Full read-only snapshot of ONE ship: hull and shield (percent + MJ), consumables and ammo (missiles, mines, countermeasures, drones, satellites, deployables), the ship's current and queued orders/tasks, and its equipment loadout (weapons, turrets, shields, engines, thrusters). Use for questions like 'what weapons does the Ray carry', 'what is my ship's current task', 'how damaged is that ship', or 'what consumables does it have'.","parameters":{"type":"object","properties":{"ship":{"type":"string","description":"ship name to search, or a numeric ship id; omit for the ship the player is currently piloting"}}}}},
 {"type":"function","function":{"name":"station_build_plan","description":"A station's construction status: planned/queued modules, the build queue (per-module price, whether resources are missing, queue position), docked builders plus reserved/incoming builders (ships with a construction order), existing modules, and the aggregated wares needed to build everything queued. Use for 'what is my station building', 'what modules are planned', 'who is my station's builder', or 'what do I need to build X' questions.","parameters":{"type":"object","properties":{"station":{"type":"string","description":"station name to search; omit for the first player station"}}}}},
 {"type":"function","function":{"name":"lookup_entry","description":"Search the in-game encyclopedia (ships, factions, wares, timeline, ...) by keyword. Use for lore questions.","parameters":{"type":"object","properties":{"query":{"type":"string"},"count":{"type":"integer"}}}}}
])";

// Build the chat body and run the whole call, including the tool-calling loop.
// Worker thread: performs network + model work; game queries are delegated to
// the UI thread via g_queries and the results are waited on.
static void llm_worker(std::string prompt) {
    load_settings(); // live-tune: pick up the latest x4llm_settings.txt each request
    std::string host, base; int port = 0;
    if (!parse_llm_url(g_llm_url.c_str(), host, port, base)) {
        logi("x4llm: bad X4MP_LLM_URL '%s'", g_llm_url.c_str());
        { std::lock_guard<std::mutex> lk(g_resp_mutex); g_resp = "LLM URL misconfigured."; }
        g_resp_ready = true; g_busy = false;
        return;
    }

    nlohmann::json messages = nlohmann::json::array();
    messages.push_back({{"role","system"},{"content", g_llm_system + TOOL_GUIDANCE}});
    messages.push_back({{"role","user"},{"content", prompt}});

    std::string final_text;
    bool finished = false;
    for (int round = 0; round <= TOOL_MAX_ROUNDS && !finished; ++round) {
        nlohmann::json body;
        body["model"] = g_llm_model;
        body["messages"] = messages;
        body["max_tokens"] = g_llm_max_tokens;
        body["temperature"] = g_llm_temperature;
        body["top_k"] = g_llm_top_k;
        body["top_p"] = g_llm_top_p;
        body["min_p"] = g_llm_min_p;
        body["presence_penalty"] = g_llm_presence_penalty;
        try { body["tools"] = nlohmann::json::parse(TOOLS_JSON); }
        catch (const std::exception& e) { logi("TOOLS_JSON parse failed (tool-calling disabled this run): %s", e.what()); }
        body["chat_template_kwargs"] = {{"enable_thinking", g_llm_enable_thinking != 0}};

        std::string resp;
        logi("x4llm: LLM round %d (%zu msgs, body %zu bytes)", round, messages.size(), body.dump().size());
        if (!llm_http(host, port, base + "/chat/completions", body.dump(), resp)) {
            final_text = "[LLM error] request failed"; finished = true; break;
        }
        // llm_http returns the raw HTTP response INCLUDING the status line and
        // headers; strip everything up to the blank line to isolate the JSON body.
        size_t se = resp.find("\r\n\r\n");
        const char* sep = "\r\n\r\n";
        if (se == std::string::npos) { se = resp.find("\n\n"); sep = "\n\n"; }
        std::string status = resp.substr(0, se == std::string::npos ? resp.size() : se);
        size_t nl = status.find_first_of("\r\n");
        if (nl != std::string::npos) status = status.substr(0, nl);
        std::string jbody = (se == std::string::npos) ? resp : resp.substr(se + strlen(sep));
        nlohmann::json j;
        try { j = nlohmann::json::parse(jbody); }
        catch (...) {
            logi("x4llm: parse failed (%s); raw=%.300s", status.c_str(), resp.c_str());
            final_text = "[LLM error] could not parse model response: " + status;
            finished = true; break;
        }
        if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()
            || !j["choices"][0].contains("message")) {
            final_text = "[LLM error] unexpected response shape"; finished = true; break;
        }
        nlohmann::json msg = j["choices"][0]["message"];

        bool has_tools = msg.contains("tool_calls") && msg["tool_calls"].is_array() && msg["tool_calls"].size() > 0;
        if (has_tools) {
            messages.push_back(msg);
            std::unique_lock<std::mutex> lk(g_qm);
            struct Pend { int qid; std::string call_id; };
            std::vector<Pend> pend;
            for (const auto& tc : msg["tool_calls"]) {
                ToolQuery q;
                q.id = g_qseq++;
                q.name = tc.value("function", nlohmann::json::object()).value("name", "");
                std::string args_s = tc.value("function", nlohmann::json::object()).value("arguments", "{}");
                try { q.args = nlohmann::json::parse(args_s); } catch (...) { q.args = nlohmann::json::object(); }
                pend.push_back({q.id, tc.value("id", "call_" + std::to_string(q.id))});
                g_queries.push_back(std::move(q));
            }
            auto all_done = [&]{
                for (const auto& p : pend) {
                    bool found = false, done = false;
                    for (const auto& q : g_queries) if (q.id == p.qid) { found = true; done = q.done; break; }
                    if (!found || !done) return false;
                }
                return true;
            };
            bool timedout = !g_qcv.wait_for(lk, std::chrono::milliseconds(TOOL_QUERY_TIMEOUT_MS), all_done);
            for (const auto& p : pend) {
                std::string result = "{\"error\":\"tool not executed\"}";
                for (auto& q : g_queries) if (q.id == p.qid) { result = q.result; break; }
                if (timedout) result = "{\"error\":\"timed out waiting for game\"}";
                nlohmann::json tm;
                tm["role"] = "tool"; tm["tool_call_id"] = p.call_id; tm["content"] = result;
                messages.push_back(tm);
                logi("x4llm: tool result #%d (%zu bytes): %.160s", p.qid, result.size(), result.c_str());
            }
            // Erase executed queries to bound memory.
            g_queries.erase(std::remove_if(g_queries.begin(), g_queries.end(),
                [&](const ToolQuery& q){ for (const auto& p : pend) if (q.id == p.qid) return true; return false; }),
                g_queries.end());
            continue; // next model round
        }

        final_text = msg.value("content", "");
        if (final_text.empty()) final_text = "[LLM returned no content]";
        finished = true;
    }

    logi("x4llm: response ready (%zu chars): %.160s", final_text.size(), final_text.c_str());
    { std::lock_guard<std::mutex> lk(g_resp_mutex); g_resp = final_text; }
    g_resp_ready = true;
    g_busy = false;
}

// ---- Event handlers ------------------------------------------------------
// Framework marks the universe fully built -> safe to query game entities.
static void on_universe_ready(const char* /*ev*/, void* /*data*/, void* /*ud*/) {
    g_universe_ready = true;
    logi("x4llm: universe ready (game context now available)");
}

// Lua -> C++ ("x4llm.ask"), param = prompt string. Runs on the UI thread, so
// the game-context snapshot (entity queries) is legal to build here.
static void on_ask(const char* /*ev*/, void* data, void* /*ud*/) {
    if (!g_api) return;
    if (g_busy) { logi("x4llm: already busy, ignoring new ask"); return; }
    std::string prompt = (data && *(const char*)data) ? (const char*)data : "hello";
    std::string ctx = build_context_snapshot(prompt);
    if (!ctx.empty()) {
        prompt = ctx + "\nUSER QUESTION: " + prompt;
        logi("x4llm: game context attached (%zu chars)", ctx.size());
    } else {
        logi("x4llm: no game context (universe not ready or faction unknown)");
    }
    g_busy = true;
    // Detached worker so the UI thread is never blocked by the LLM call.
    std::thread(llm_worker, std::move(prompt)).detach();
}

// Main thread: execute queued tool queries, then flush any completed response.
static void on_frame_update(const char* /*ev*/, void* /*data*/, void* /*ud*/) {
    // Game entity APIs must run on the UI thread; drain whatever the worker queued.
    {
        std::lock_guard<std::mutex> lk(g_qm);
        bool any = false;
        for (auto& q : g_queries) {
            if (!q.done) {
                q.result = dispatch_tool(q.name, q.args);
                q.done = true;
                any = true;
            }
        }
        if (any) g_qcv.notify_all();
    }
    if (!g_api || !g_api->raise_lua_event) return;
    if (!g_resp_ready) return;
    std::string text;
    { std::lock_guard<std::mutex> lk(g_resp_mutex); text = std::move(g_resp); g_resp.clear(); }
    g_resp_ready = false;
    g_api->raise_lua_event("x4llm.response", text.c_str());
}

// ---- Exported entry points ----------------------------------------------
X4NATIVE_EXPORT int x4native_api_version(void) { return X4NATIVE_API_VERSION; }

X4NATIVE_EXPORT int x4native_init(X4NativeAPI* api) {
    g_api = api;
    g_game = api->game;

    if (const char* e = std::getenv("X4MP_LLM_URL"))       { if (*e) g_llm_url = e; }
    if (const char* e = std::getenv("X4MP_LLM_MODEL"))     { if (*e) g_llm_model = e; }
    if (const char* e = std::getenv("X4MP_LLM_SYSTEM"))    { if (*e) g_llm_system = e; }
    if (const char* e = std::getenv("X4MP_LLM_MAXTOKENS")) { int v = atoi(e); if (v > 0) g_llm_max_tokens = v; }
    if (const char* e = std::getenv("X4MP_LLM_TEMPERATURE")){ double v = atof(e); if (v >= 0 && v <= 2) g_llm_temperature = v; }

    // Lua raise_event("x4llm.ask", prompt) -> C++ bus "x4llm_ask".
    if (api->register_lua_bridge) api->register_lua_bridge("x4llm.ask", "x4llm_ask");
    if (api->subscribe) {
        api->subscribe("x4llm_ask", on_ask, nullptr, api);
        api->subscribe("on_frame_update", on_frame_update, nullptr, api);
        api->subscribe("on_universe_ready", on_universe_ready, nullptr, api);
    }
    logi("x4llm: initialized (LLM=%s model=%s maxtok=%d)", g_llm_url.c_str(), g_llm_model.c_str(), g_llm_max_tokens);
    return X4NATIVE_OK;
}

X4NATIVE_EXPORT void x4native_shutdown(void) { g_api = nullptr; g_game = nullptr; }
