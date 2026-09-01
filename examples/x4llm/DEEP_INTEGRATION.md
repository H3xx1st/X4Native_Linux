# x4llm — Deep Game Integration (Phases 1–3)

Makes the X4 board computer ("AI Board Computer") aware of the player's live
state AND the in-game encyclopedia, and able to **query the game on demand via
tool-calling**. Everything is built in the C++ extension (`x4llm.cpp`); the Lua
UI (`ui/x4llm.lua`, `ui/x4llm_chat.lua`) is unchanged and shared across platforms.

Status snapshot: **tool-calling implemented, built, and deployed to Linux
(`x4llm_linux`). In-game test pending.** The encyclopedia index has a known
coverage gap (see "Encyclopedia index" below) that is the top next task.

## Architecture

### Request flow
`on_ask` (UI thread) builds an orientation snapshot, spawns a **detached
worker thread**, which runs the tool-calling loop and posts the final answer via
`raise_lua_event("x4llm.response", ...)`. `on_frame_update` (UI thread) drains
the tool-query queue and flushes the response.

### Tool-calling loop (Phase 3 — the dynamic part)
The model is given a curated set of **read-only tools** (`TOOLS_JSON`) plus
`TOOL_GUIDANCE` appended to the system prompt telling it *not to guess, call the
right tool first*. `llm_worker` runs a multi-turn loop (max `TOOL_MAX_ROUNDS=8`):

```
POST {messages, tools, ...}  ->  read choices[0].message
  if message.tool_calls:     enqueue each as a ToolQuery, wait, append results
                             as role:"tool" messages, loop
  else:                      message.content is the final answer
```

**Threading constraint (critical):** game entity APIs are NOT thread-safe and
must run on the **UI thread**, but the model runs on a worker thread. So the
worker enqueues `ToolQuery{name,args}` onto `g_queries` and blocks on
`g_qcv` (with `TOOL_QUERY_TIMEOUT_MS` timeout); `on_frame_update` (UI thread)
executes each via `dispatch_tool()` and posts the result. This producer/consumer
is the main design point — never call game APIs from the worker directly.

### Current tools (all read-only)
- `player_ship` — `GetPlayerObjectID` + `GetComponentName/Class` + owner. Solves
  "what ship am I in?" (returns `ship=null` when docked).
- `my_ships` — `GetNumAllFactionShips`+`GetAllFactionShips` grouped by class + counts.
- `my_stations` — `GetAllFactionStations` (name + class).
- `my_sectors` — `GetSectorsByOwner`.
- `ship_status` — full read-only snapshot of ONE ship (arg `ship` = name or id,
  omit → current ship): hull/shield % via `GetComponentDetails(id,"")`
  (`ComponentDetails{name,hull,shield,speed,hasShield}`) + MJ current/max via bare-global
  `GetComponentData(id,"hull","hullmax","shield","shieldmax")` (Lua bridge); consumables via
  `GetNum/GetAll*` (`AmmoData{macro,ware,amount,capacity}`) for missiles/mines/countermeasures/
  satellites/navbeacons/resourceprobes/lasertowers + `GetAllUnits` (`UnitData`) for drones;
  orders via `GetNumOrders`+`GetOrders3` (`Order3{orderdef,state,statename,queueidx,enabled}`),
  active order from `GetOrderQueueCurrentIdx`, human names from `GetOrderDefinition`;
  loadout via `GetNumUpgradeGroups`+`GetUpgradeGroups2`+`GetUpgradeGroupInfo2` (grouped
  turrets/shields/engines — each group yields `.currentmacro` + `.count`, aggregated into an
  accurate per-component name→count breakdown) with `GetNumUpgradeSlots`+`GetUpgradeSlotCurrentMacro`
  as the individual-slot fallback (weapons), plus virtual `GetNumVirtualUpgradeSlots("thruster")`.
  Macro ids are named through `macro_name()` → bare-global `GetMacroData(macro,"name")`.
  Orders: `GetDefaultOrder(id)` is REQUIRED in addition to the queue — a ship's standing task
  (e.g. "Protect Station") is its DEFAULT behaviour and is NOT returned by GetNumOrders/GetOrders3
  when the queue is empty; when queue is empty the default order is reported as the current task.
  NOTE: per-ship ENERGY and HEAT have NO API (player-HUD-only `GetBoostEnergyPercentage` /
  `GetWeaponDetails3`); energy cells are a plain cargo ware, heat is HUD-only. So consumables
  = ammo/deployables only, not energy/heat.
- `station_build_plan` — `GetNumBuildTasks`/`GetBuildTasks` (BuildTaskInfo has
  `macro`, `price`, `ismissingresources`, `queueposition`) + `GetBuildResources`
  per macro (`UIWareInfo{ware,amount}`) + `GetContainerBuildResources` (active
  build wares). Solves "what do I need to build X at my station?".
  RESERVED BUILDERS: `GetAssignedConstructionVessels` only counts DOCKED CVs. To also name a
  CV that's assigned but inbound, scan every player ship's `GetOrders3` for an
  `orderdef` containing `"DeployToStation"` and report those as `reserved_builders` (ship-side
  is the only API — see the Build-plan section below).
- `lookup_entry` — encyclopedia keyword search (`search_encyclopedia`).

Player faction id resolution uses a fallback chain (`x4llm_faction_id`):
`GetPlayerFactionName(true)` → `GetPlayerObjectID`+`GetOwnerDetails2().factionID`
→ literal `"player"`. `GetPlayerFactionName` was observed to return **empty** on
some Linux runs even when the universe is ready — the fallback is why live
context works at all.

## LLM server requirements
- **llama.cpp with `--jinja`** (enables the model's native chat template).
- A **tool-aware chat template** that (a) renders the `tools` array and
  (b) renders `role:"tool"` result messages. User's template:
  `~/Downloads/qwen3.8-27b-jinanew.jinja` — verified to do both (handles
  `message.tool_calls` and `role == 'tool'`, and integrates with reasoning).
- `--reasoning-format deepseek` (their setup). Model: Salience-27B-R5 (Qwen3
  fine-tune). Caveat: if the fine-tune was trained without tool use, the model
  may occasionally skip tools — check the log for `x4llm: LLM round N` /
  `x4llm: tool result #N` lines to confirm rounds fired.

## Encyclopedia index
`tools/build_encyclopedia_index.py` writes `data/encyclopedia.index`
(category \x1f id \x1f name \x1f description, one record per line).
**Currently 4248 entries** (ships 377, station modules 397, jobs 564, …).

### Coverage status (2026-08-28)
Fixed: encyclopedia records are `<identification>` elements (`name="{page,line}"`,
optional `description="{page,line}"`, `page=`, `makerrace=` — **NO `id`**). The
indexer now makes `id` optional (dedups on `(tag, id-or-nameref)`) and greps
`<identification>` out of the LARGE archives (`IDENT_ARCHIVES = 01/02/05.dat`,
≈10 GB — must grep, never `f.read()`) in addition to full-scanning `08.dat`. This
took ships 177→377.

**Remaining gap — Terran + Boron ships are MISSING.** The readable archives only
contain identifications for argon/teladi/paranid/khaak/xenon/split (0 terran,
0 boron ships). Their data sits in a **compressed section** the offline grep
can't read. Confirmed: the Walrus name text exists in the language file
(`t/0001-l044.xml`, page 20101 line 52901) and is cross-referenced by other
ships, but NO readable element names `{20101,52901}`; the packed members
(`libraries/character_components.xml`, `index/components.xml`) contain 0
identifications. So the terran/boron ship identification elements are in a
compressed member not yet located.

**Next task to close the gap:** find the x4cat member that holds the terran/boron
ship `<identification>` elements (try `x4cat extract` on candidates and grep for
`makerrace="terran"`), then add it to `IDENT_ARCHIVES`-style extraction. NOTE:
line ids are NOT unique across pages (page 20101 line 52901 = "Walrus" but page
20104 line 52901 = "Venture Base Connection Structure"); always resolve the
full `{page,line}` pair.

### Build-plan tool (`station_build_plan`) — CORRECT API (verified vs game UI Lua)
The station's queued modules are read **without a holomap** via:
- `GetNumPlannedStationModules(sid, includeall=true)` → `GetPlannedStationModules(result, resultlen, sid, includeall=true)`
  — each `UIConstructionPlanEntry` has `.macroid` (the queued module). This is the
  authoritative "planned modules" list.
- **BUILDSTORAGE container**: builders + build tasks live on the station's buildstorage,
  NOT the station id. Get it via the typed `g->GetContextByClass(sid, "buildstorage", false)`
  (GameClass::Buildstorage = 8, name string "buildstorage" — see x4_game_class_ids.inc).
  This is why `GetAssignedConstructionVessels(sid)`/`GetBuildModules(sid)` failed before.
- Build TASKS (status/builder) are keyed by **buildmodule**: `GetNumBuildModules(storage)` →
  `GetBuildModules(result, resultlen, storage)`, then per buildmodule
  `GetBuildTasks(result, resultlen, storage, buildmodule, isinprogress=true, includeupgrade=true)`.
  `BuildTaskInfo` carries `.macro`, `.price`, `.ismissingresources`, `.queueposition`,
  `.buildercomponent` (assigned builder — name it with GetComponentName).
  Try the buildstorage first, fall back to `sid`.
- BUILDERS: `GetNumAssignedConstructionVessels(storage)` / `GetAssignedConstructionVessels(result, resultlen, storage)` —
  matches the config menu (`GetAssignedConstructionVessels(…, menu.buildstorage)`).
  CAVEAT: this only counts vessels that are ACTIVELY building/docked — it returns 0 when a
  ship is merely *assigned/reserved* but not docked (there is NO separate "reserved vessels"
  API in the typed func table). The buildstorage is resolved three ways and dedup'd:
  `GetContextByClass(sid, "buildstorage")`, `GetComponentData(sid, "buildstorage")` (the UI's
  exact source, via the Lua bridge `get_lua_property_str`), and `sid` itself. A diagnostic line
  `station_build_plan buildstorage candidates=[…] total_assigned=N` is logged: N>0 but no
  builder names ⇒ name-resolution bug; N==0 ⇒ Walrus assigned-but-not-docked (expected).
  Empty build queue (`GetBuildTasks`=0) likewise just means nothing is actively building right
  now — it populates only when construction is queued/running.
- RESOURCE totals: prefer the confirmed build sequence (build-task macros = the modules
  actually being built = the UI's "changes") for accuracy; fall back to all planned modules.
  The UI's "Planned and Changed Modules" counts only the NEW modules, whereas
  `GetPlannedStationModules(sid, includeall=true)` also includes already-built base
  modules — summing recipes over all of them overstates the totals.
- Required wares (per-module, WITH multiplicity): read each module's BUILD RECIPE
  from the engine ware DB (works out-of-UI AND for Boron/Terran modules, whose defs are
  compressed in the data files but fully loaded at runtime). Chain:
  1. macro → module ware: Lua global `GetMacroData(macro, "ware")` (NOT in the g-> table).
  2. ware → build method: first entry of Lua global `GetWareData(ware, "productionmethods")` (a table).
  3. method → recipe wares: `g->GetNumProductionMethodResources(ware, method)` then
     `g->GetProductionMethodResources(UIWareAmount* res, n, ware, method)` — `UIWareAmount{wareid, amount}`.
  Because steps 1-2 need the Lua bridge and `get_lua_property(_str)` only returns scalars, a
  GLOBAL helper is defined in `x4llm.lua` — `X4LLM_build_target(macro, _)` returns the
  scalar string `"ware|method"` — and C++ calls it via
  `g_api->get_lua_property_str("X4LLM_build_target", {X4N_KEY_STRING, macro}, "", buf, …)`,
  then parses `ware|method` and does the typed recipe call. Iterate `planned` (one entry per
  module, so 4× counts 4×) and sum into `total_required_wares`.
  NOTE: `Water` in the config menu is engine-derived from the module's `<workforce>` (crew
  supplies), NOT in the production recipe — the recipe yields the structural wares
  (Claytronics / Hull Parts / …). Verify at runtime which wares actually come back.

WRONG/DEAD ENDS (do not retry):
- `GetBuildMapConstructionPlan(holomapid, defensibleid, usestoredplan, …)` and the
  `…Plan2` variant are GUARDED by `if holomap ~= 0` in the game UI — they only work
  while a station's construction **holomap** is open. Out-of-UI they return 0.
  (My first attempt also had the first two args swapped: defensible=station goes
  SECOND, holomapid=0 FIRST.) Use `GetPlannedStationModules` instead.
- `GetBuildResources(…, macroname, ware)` needs an ACTIVE build: it errors "Failed to
  retrieve buildmodule of container ... suitable for macro/defensible ..." when nothing is
  queued. Empty wareid errors too. NOT usable for merely-planned modules.
- `GetModuleNeededResources(holomap, idx)` is holomap-gated — unusable out-of-UI.
- `GetWareData(ware, "resources")` is NOT a real field (Strix guessed it; the game never
  calls it) — use `GetWareData(ware, "productionmethods")` + `GetProductionMethodResources`.
- `get_lua_property_str` returns scalars only; Lua tables (like "productionmethods") must be
  reduced to a string by a custom global helper in x4llm.lua.
- `GetBuildTasks` with `buildmodule=0` returns nothing — you MUST pass a real
  buildmodule id from `GetBuildModules`.
- `GetAssignedConstructionVessels(result, len, sid)` errors ("Failed to retrieve
  buildmodule from container") — get builders from `BuildTaskInfo.buildercomponent` instead.

Signatures all come from `x4_game_func_list.inc`; call them through the func
pointer struct as `g->Fn(…)` (guard each with `if (g->Fn)`). The typed wrappers
in `x4n_station.h`/`x4n_module.h` are available but don't expose the
planned-modules / build-task APIs, so use the raw func pointers here.

### Dedup rule (do not regress)
Key on `(tag, id-or-nameref)` — never id alone (ids collide across element
types, e.g. `"xenon"` is a `<faction>`, `<race>`, and a `<compatibilitytype>`).

## Encyclopedia unlock feature (pure Lua, pending evening test)
A main-menu row **"AI — Unlock Encyclopedia"** makes every encyclopedia entry known
to the player so the in-game encyclopedia shows the full catalogue. Implemented in
`ui/x4llm.lua` (`unlock_encyclopedia()`), triggered from the `submenuHandler` (ROW_UNLOCK).
No C++ rebuild — pure Lua.

### Mechanism (verified against `menu_encyclopedia.lua` + `08.dat` call sites)
- **Unlock primitive**: bare Lua global `AddKnownItem(librarytype, itemid)` — inserts
  `(type, id)` into the known-items DB with read-flag 0. Idempotent (dedup-safe),
  no-op for unknown ids. NOT in the `g->` func table (engine-registered Lua global,
  like GetWareData). `ReadAllKnownItems()` at the end clears the "new" badges.
- **Enumeration** (ffi.C, mirroring the encyclopedia menu):
  1. Wares: `GetNumWares/GetWares("", research, "", "hidden")` for research=false AND
     true (empty tags = all wares). Per ware: `AddKnownItem("wares", ware)` +
     `AddKnownItem("inventory_wares", ware)`; then `GetWareData(ware, "component", "productionmethod")`
     → macro + production method → `AddKnownItem(lib, macro)` where
     `lib = GetMacroData(macro, "infolibrary")` (this unlocks the macro's actual
     encyclopedia category: shiptypes_*, moduletypes_*, stationtypes, weapons_*, …).
  2. Factions: `GetAllFactions(true)` → `AddKnownItem("factions", f)` +
     `GetOwnLicences(f)` → `AddKnownItem("licences", lic.id)`.
  3. Races: `GetAllRaces()` (RaceInfo{.id,…}) → `AddKnownItem("races", rid)`.
  4. Galaxy: `GetClusters(true)`/`GetSectors(cluster)` → `SetKnownTo(id, "player")`
     (component-level known, not library items).
  Each phase is pcall'd + logged (`ency-unlock … ERR`) so partial data still works.
- ffi.cdef: declare each symbol individually inside `pcall(ffi.cdef, decl)` to
  tolerate symbols already declared by loaded menus (LuaJIT cdef errors on redefinition).
- **Do NOT use** `RevealEncyclopedia()` — it's a single-`ret` no-op stub on the
  Linux ELF (verified via objdump @0xC00020). Same for `RevealMap`.

### Caveats / known gaps
- `GetWares("", …)` semantics (empty tags = all) is a best guess; if the evening
  test logs `wares=0`, switch to the menu's per-tag passes (equipmenttags/consumabletags
  etc., menu_encyclopedia.lua:640–715).
- Some library types have no runtime enumeration (audiologs, timeline, marines,
  unique_npcs, entitytypes) — those need offline id harvest like the encyclopedia
  indexer. Minor for now.
- This unlocks the PLAYER's in-game encyclopedia. The LLM's `lookup_entry` tool uses
  the OFFLINE index independently (so it works without this). To close the LLM's
  Terran/Boron lore gap, a follow-up would route `lookup_entry` through the live
  `GetLibraryEntry(type, id)` (resolves ANY entry regardless of known state).

## Deployment
Linux `.so` deploys to `extensions/x4llm/native/x4llm.so` in each game install +
the `x4llm_linux` ship package; `data/encyclopedia.index` alongside. Windows
`.dll` is cross-built from the **same** `x4llm.cpp` (the source is shared; the
`.cpp` carries its own `#ifdef _WIN32` winsock/WSAStartup handling). To rebuild:
sync `x4llm.cpp` to the Windows source tree, then
`cd <linux examples>/x4llm && cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=../mingw-w64-x86_64.cmake
-DCMAKE_BUILD_TYPE=Release && cmake --build build-win` → `build-win/x4llm.dll`.
Deploy to BOTH `native/x4llm.dll` and `native/x4llm_live.dll` in every Windows install
(see the `x4native-windows-mingw-crossbuild` skill). As of the ship_status/orders/
reserved-builder work, Windows is fully in sync with Linux.

## Linux facts (verified)
- Game binary `X4` exports ~28 644 symbols; the framework resolved **2065/2065**
  game functions. Internal RVA/byte-signature resolution is **disabled on Linux**
  (no exported symbol base) — everything we use is a normal exported symbol.
- Log: `<profile>/EgoSoft/X4/x4native/x4native.log` (framework routes extension
  `log()` here; the per-extension `x4native/x4llm/x4llm.log` only holds the header).
- Relative paths (`extensions/x4llm/...`) resolve against the game dir.

## Config
`x4llm_settings.txt` (live-tuned each request): `url`, `model`, `temperature`,
`top_k`, `top_p`, `min_p`, `presence_penalty`, `enable_thinking`, … Env overrides:
`X4MP_LLM_URL`, `X4MP_LLM_MODEL`, `X4MP_LLM_SYSTEM`, `X4MP_LLM_MAXTOKENS`,
`X4MP_LLM_TEMPERATURE`, `X4LLM_ENCY_INDEX`, `X4MP_LLM_SETTINGS`.

## Build
```
cd examples/x4llm
cmake -S . -B build && cmake --build build --target x4llm   # Linux -> x4llm.so
```
`CMakeLists.txt` adds `../../third_party` (for nlohmann/json.hpp, header-only).
