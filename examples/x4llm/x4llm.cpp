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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

// Build the chat body and run the whole call (worker thread).
static void llm_worker(std::string prompt) {
    load_settings(); // live-tune: pick up the latest x4llm_settings.txt each request
    std::string host, base; int port = 0;
    if (!parse_llm_url(g_llm_url.c_str(), host, port, base)) {
        logi("x4llm: bad X4MP_LLM_URL '%s'", g_llm_url.c_str());
        std::string err = "LLM URL misconfigured.";
        { std::lock_guard<std::mutex> lk(g_resp_mutex); g_resp = err; }
        g_resp_ready = true; g_busy = false;
        return;
    }
    char tail[512];
    snprintf(tail, sizeof(tail),
        ",\"max_tokens\":%d"
        ",\"temperature\":%.4f"
        ",\"top_k\":%d"
        ",\"top_p\":%.4f"
        ",\"min_p\":%.4f"
        ",\"top_n_sigma\":%.4f"
        ",\"presence_penalty\":%.4f"
        ",\"typical_p\":%.4f"
        ",\"repeat_penalty\":%.4f"
        ",\"chat_template_kwargs\":{\"enable_thinking\":%s}}",
        g_llm_max_tokens,
        g_llm_temperature,
        g_llm_top_k,
        g_llm_top_p,
        g_llm_min_p,
        g_llm_top_n_sigma,
        g_llm_presence_penalty,
        g_llm_typical_p,
        g_llm_repeat_penalty,
        g_llm_enable_thinking ? "true" : "false");
    std::string body = "{\"model\":\"" + jesc(g_llm_model) + "\""
        ",\"messages\":[{\"role\":\"system\",\"content\":\"" + jesc(g_llm_system) + "\"}"
        ",{\"role\":\"user\",\"content\":\"" + jesc(prompt) + "\"}]"
        + tail;

    std::string resp;
    logi("x4llm: calling LLM %s:%d %s (prompt %zu chars)", host.c_str(), port, (base + "/chat/completions").c_str(), prompt.size());
    bool ok = llm_http(host, port, base + "/chat/completions", body, resp);
    std::string text;
    if (!ok || !extract_content(resp, text) || text.empty()) {
        // Surface a short diagnostic (first 200 chars of the raw response).
        char diag[256];
        snprintf(diag, sizeof(diag), "[LLM error] %s\n---\n%.200s",
                 ok ? "empty content" : "request failed", resp.c_str());
        text = diag;
    }
    logi("x4llm: response ready (%zu chars): %.160s", text.size(), text.c_str());
    { std::lock_guard<std::mutex> lk(g_resp_mutex); g_resp = text; }
    g_resp_ready = true;
    g_busy = false;
}

// ---- Event handlers ------------------------------------------------------
// Lua -> C++ ("x4llm.ask"), param = prompt string.
static void on_ask(const char* /*ev*/, void* data, void* /*ud*/) {
    if (!g_api) return;
    std::string prompt = (data && *(const char*)data) ? (const char*)data : "hello";
    if (g_busy) { logi("x4llm: already busy, ignoring new ask"); return; }
    g_busy = true;
    // Detached worker so the UI thread is never blocked by the LLM call.
    std::thread(llm_worker, std::move(prompt)).detach();
}

// Main thread: flush any completed response to Lua.
static void on_frame_update(const char* /*ev*/, void* /*data*/, void* /*ud*/) {
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
    }
    logi("x4llm: initialized (LLM=%s model=%s maxtok=%d)", g_llm_url.c_str(), g_llm_model.c_str(), g_llm_max_tokens);
    return X4NATIVE_OK;
}

X4NATIVE_EXPORT void x4native_shutdown(void) { g_api = nullptr; g_game = nullptr; }
