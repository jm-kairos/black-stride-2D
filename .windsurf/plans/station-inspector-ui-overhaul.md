# Station Inspector UI Overhaul — Tabbed Interface

## Goal
Replace the flat station inspector (two text lines) with a tabbed interface reusing the ship inspector's `.insp-tabs` / `.insp-tab` style. Tabs correspond to station rooms: Dock, Market, Contracts.

## Data Flow (existing pattern)
1. `game_state.h` — POD state fields (bools, ints, fixed arrays)
2. `bs_rml.h` — `bs_rml_hud_state` snapshot struct (POD, copied per frame)
3. `renderer_backend_sdlgpu.cpp` — `BsRmlHudModel` (Rml::String/Vector mirrors), bindings, per-frame copy
4. `hud.rml` — RML document using `data-if`, `data-for`, `{{ }}` bindings
5. `hud.rcss` — styling
6. `game.cpp` — populates snapshot from game state, drains `hud_action` queue

## Steps

### 1. Game state: add tab tracking (`game_state.h`)
- Add `i32 station_insp_tab` to `game_state` (0=Dock, 1=Market, 2=Contracts; default 0)

### 2. HUD state: add tab + list data (`bs_rml.h`)
- Add `i32 station_insp_tab` (active tab index)
- Add `b8 station_insp_tab_visible[3]` (which tab buttons to show)
- Add docked-ships list:
  - `#define BS_RML_STATION_DOCK_MAX 8`
  - `typedef struct bs_rml_station_dock_line { char name[64]; char status[32]; } bs_rml_station_dock_line;`
  - `i32 station_insp_dock_count;`
  - `bs_rml_station_dock_line station_insp_dock[BS_RML_STATION_DOCK_MAX];`
- Add market list:
  - `#define BS_RML_STATION_MARKET_MAX 4`
  - `typedef struct bs_rml_station_market_line { char name[32]; char stock[32]; char price[32]; } bs_rml_station_market_line;`
  - `i32 station_insp_market_count;`
  - `bs_rml_station_market_line station_insp_market_list[BS_RML_STATION_MARKET_MAX];`
- Add contracts list:
  - `#define BS_RML_STATION_CONTRACT_MAX 8`
  - `typedef struct bs_rml_station_contract_line { char dest[64]; char cargo[32]; char reward[32]; char stage[32]; } bs_rml_station_contract_line;`
  - `i32 station_insp_contract_count;`
  - `bs_rml_station_contract_line station_insp_contract[BS_RML_STATION_CONTRACT_MAX];`
- Remove old `station_insp_rooms[96]` and `station_insp_market[160]` string fields

### 3. Renderer model: mirror + bind + assign (`renderer_backend_sdlgpu.cpp`)
- Add `BsRmlHudStationDock`, `BsRmlHudStationMarket`, `BsRmlHudStationContract` structs (Rml::String fields)
- Add corresponding `Rml::Vector<>` members to `BsRmlHudModel`
- Add `i32 station_insp_tab` and `bool station_insp_tab_visible[3]` to model
- Bind all new variables in `bs_rml_hud_init`
- Copy data in `bs_rml_hud_update` (same pattern as arsenal_inv)

### 4. RML: tabbed station inspector (`hud.rml`)
- Replace the two `station-insp-line` divs with:
  - Tab bar: `.insp-tabs` with `data-if` per tab button, `data-class-active` based on `station_insp_tab`, `data-event-click="hud_action('station_tab:N')"`
  - Tab content panels using `data-if`:
    - Dock: `data-for="d : station_insp_dock"` showing name + status
    - Market: `data-for="m : station_insp_market_list"` showing name, stock, price
    - Contracts: `data-for="c : station_insp_contract"` showing dest, cargo, reward, stage

### 5. RCSS: station inspector tab + list styles (`hud.rcss`)
- Reuse `.insp-tabs`, `.insp-tab`, `.insp-tab.active` (already defined)
- Add `.station-insp-line` styles for list rows (label + value pairs)
- Add `.station-insp-empty` style for "no entries" message

### 6. Game logic: populate lists + handle tab actions (`game.cpp`)
- In the station inspector HUD snapshot section:
  - Set `station_insp_tab` from `s->station_insp_tab`
  - Set `station_insp_tab_visible[]` based on `station_rooms()` result
  - **Dock tab**: Iterate NpcShips, find those with `state == AI_TRADE_DOCKED` whose mission's current dock station matches `inspect_station_id`. Also check macro missions in ORIGIN_DOCK/MARKET_DOCK at this station (for when player is not in-system). Fill name + status.
  - **Market tab**: Call `station_market_get()`, fill name/stock/price per good.
  - **Contracts tab**: Iterate `galaxy.missions[]`, find those with `station_id == inspect_station_id`. Fill dest system name, cargo good name, units, reward, stage name.
- In the action drain loop:
  - Handle `"station_tab:0"`, `"station_tab:1"`, `"station_tab:2"` → set `s->station_insp_tab`
- On `station_inspect` action: reset `s->station_insp_tab = 0` (default to Dock)

### 7. Build and verify
- `cmd /c "cd /d C:\dev\blackstride && build-all.bat"`
- Right-click station → Inspect → verify tabs appear, switching works, content populates
