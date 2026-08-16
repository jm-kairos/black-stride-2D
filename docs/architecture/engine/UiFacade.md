# UiFacade

**Responsibility:** Owns the SDL-free, third-party-free control surfaces for Dear ImGui and
RmlUi — lifecycle, event feeding, input-capture queries, and the in-game HUD data model. Its
purpose is negative: to let the rest of the engine and the whole game drive ImGui and RmlUi
without ever seeing an SDL, ImGui or RmlUi type. It explicitly does not own the per-frame
`NewFrame`/`Render` sequencing (deliberately withheld — RenderBackend drives it so it cannot
fall out of balance with the GPU frame), and — the important caveat — **it does not own its own
implementation**: all 29 of its functions are defined inside `renderer_backend_sdlgpu.cpp`.

**Public interface:** `engine/source/renderer/bs_imgui.h` — `bs_imgui_initialize`, `_shutdown`,
`_process_event` (takes an `SDL_Event*` as `const void*`), `_wants_mouse`, `_wants_keyboard`,
`_get_hud_font` (returns an `ImFont*` as `void*`).
`engine/source/renderer/bs_rml.h` — `bs_rml_document` (opaque `{void* ptr;}`);
`bs_rml_initialize`, `_shutdown`, `_load_fonts`, `_load_document`, `_show`, `_unload_document`,
`_update`, `_process_event`, `_wants_mouse`, `_wants_keyboard`, `_on_resize`,
`_debugger_toggle`, `_set_sharpen`; and the HUD tier — `bs_rml_hud_state` (a ~150-field POD
snapshot), `bs_rml_log_line`, `bs_rml_disc_line`, `bs_rml_weapon_line`, `bs_rml_bay_line`,
`bs_rml_gm_cell`, `bs_rml_gm_row`, `bs_rml_roster_chip`, `bs_rml_roster_row`, plus
`bs_rml_hud_init`, `_hud_shutdown`, `_hud_update`, `_hud_poll_action`. All 23 functions are
`bs__api__`.

**Depends on:** Foundation (its only include).
**Depended on by:** Platform, RenderFrontend, RenderBackend, Widgets — and the sandbox, which
uses 10 of the 23 functions across 9 files.

**Key invariants:**
- **No SDL, ImGui or RmlUi type appears in either header.** Held by using `void*` in both
  directions — `bs_imgui_process_event(const void*)` (`bs_imgui.h:35`) and
  `bs_imgui_get_hud_font()` returning `void*` (`:45`). Enforced by inspection only; neither is
  type-checked.
- **Initialise after the GPU device and window exist; shut down before the device is
  destroyed.** Documented at `bs_imgui.h:13-15` and `bs_rml.h:14-15`; *enforced* by
  `renderer/renderer.cpp:75,82` (after `backend.initialize`) and `:102,106` (before
  `backend.shutdown`). The SDL_GPU ImGui backend owns device-tied resources, so violating this
  is a use-after-free.
- **Per-frame calls are deliberately absent from the API.** `NewFrame`/`Render`/record are
  sequenced inside `renderer_backend_sdlgpu.cpp`'s `begin_frame`/`end_frame`, stated at
  `bs_imgui.h:16-18` as being so they "can never fall out of balance with the GPU frame".
- **`bs_rml_update` must be called once per frame before rendering** (`bs_rml.h:54-56`); it also
  syncs the RmlUi context size to the swapchain. Satisfied by `renderer.cpp:142`.
- **The `wants_*` pair is the UI/world input arbitration contract** — callers are expected to
  gate world input on their negation, combined across both facades (`bs_imgui.h:37-39`,
  `bs_rml.h:62-63`). Nothing enforces it; a caller that ignores them gets clicks applied twice.
- **HUD interactions never mutate game state.** Clicks enqueue short action strings that the
  game drains with `bs_rml_hud_poll_action` in a loop until it returns 0 (`bs_rml.h:92-93`,
  `:323-326`).
- **`bs_rml_hud_update` copies, it does not retain.** Verified: it assigns field-by-field into
  the engine-side data model via `bs_rml_assign` (`renderer_backend_sdlgpu.cpp:4675+`).

**Extension points:** **Adding a HUD element is a three-place edit** — a field in
`bs_rml_hud_state` (`bs_rml.h:168-310`), a corresponding assignment in `bs_rml_hud_update`
(`renderer_backend_sdlgpu.cpp:4675+`), and a binding in the RML document
(`assets/ui/hud.rml`). Adding an interaction means emitting a new action string from the
document and handling it in the game's `bs_rml_hud_poll_action` drain loop
(`sandbox/source/game.cpp:1912+`); the action grammar (`"group:N"`, `"gm:W:G"`, `"inv:K"`,
`"pd:stance:N"`, `"spd:N"`, `"doc:roe:N"`, …) is documented only in `bs_rml.h`'s comments. Adding a facade function means
a `bs__api__` declaration here plus an implementation at the bottom of the backend TU.

**Known limitations / tech debt:**
- **The headers have no implementation files.** All 29 functions live in
  `renderer_backend_sdlgpu.cpp` — roughly 1866 of its 4888 lines (38%) — and they read the
  backend's `g_sdl` global directly rather than through an accessor. By *interface* this is a
  separate layer; by *code* it is the backend. This is flagged in `engine-subsystems.md` as the
  one "maybe actually one subsystem" pair. **Provisional:** whether UiFacade should stay a
  separate subsystem, merge into RenderBackend, or be extracted into real `bs_imgui.cpp` /
  `bs_rml.cpp` files is an open question.
- **`bs_rml_hud_state` encodes an enormous amount of game domain vocabulary in an engine
  header** — fire groups, point-defense stances, market specialisations, planet habitability,
  station contracts. All display formatting is game-side by explicit policy (`bs_rml.h:88-91`),
  which is why the engine header must enumerate every string the game might show.
- The whole snapshot is copied every frame regardless of what changed, and
  `bs_rml_hud_update` dirties every variable (`bs_rml.h:319-321`).
- Every field is a fixed-size `char` array, so oversized game strings are silently truncated.
- **`BS_RML_GROUP_MAX` is annotated "SHIP_WEAPON_GROUPS game-side"** (`bs_rml.h:99`) — a
  duplicated constant across the engine/game boundary with no compile-time check.
  `BS_RML_ROSTER_MAX` ("FLEET_MAX_SHIPS game-side") repeats the pattern for the fleet roster
  rows.
- **The per-frame dirty-everything in `bs_rml_hud_update` breaks per-row `data-attr-*`
  bindings.** Reapplying an attribute every frame churns the element, so a press/release pair
  never lands on one instance — clicks (and hover) silently die on any data-for row carrying a
  data-attr binding, and an `<img>` child has the same effect. Discovered building the
  inspector's fleet list; the workaround is statically authored rows with literal actions and
  decorator-painted images (text and class bindings are idempotent and safe). A real fix is
  per-variable dirtying.
- **RmlUi overflow scrolling is effectively unusable in this integration.** A container that
  actually overflows with `overflow-y: auto` loses hit-testing for its children, and a nested
  fixed-height scroll region drops its content from layout entirely. The inspector is sized so
  nothing overflows; treat `overflow` as display-only (clipping) until this is understood.
- Comments reference external design docs (`docs/POINT_DEFENSE_AND_MISSILES.md`) and a shader
  (`rml.frag.hlsl`, for the sharpening contract), so the header depends on artifacts outside the
  code.
- 13 of the 23 exported functions are never called by the sandbox — including the entire
  document API (`bs_rml_load_document`, `_show`, `_unload_document`) and the ImGui lifecycle
  four. The game only ever uses the HUD convenience path, which loads its document internally.
- `bs_imgui_get_hud_font` hands back an `ImFont*` the caller must cast, and the font itself is
  loaded from a hardcoded Windows path in the backend.

**Source paths:** `engine/source/renderer/bs_imgui.h`, `engine/source/renderer/bs_rml.h`
(implementation: `engine/source/renderer/backend/renderer_backend_sdlgpu.cpp`)

**Last verified:** 2026-08-17, working tree on `game` (`bs_rml_hud_state` gains the fleet
skill hotbar — `bs_rml_skill_slot skill[BS_RML_SKILL_MAX 9]` + `skills_visible`/`skill_count`
and the `skill_target_visible`/`skill_target_label` targeting banner, action grammar
`"skill:N"`; the cooldown sweep rides the `fleet_cap_w`-style `data-style-width` string bind,
and `BS_RML_BAY_MAX` grows 12 → 18 for the widened weapon catalog — see SkillSystem).
Previously 2026-08-16 (`bs_rml_hud_state` gains the
inspected-ship engagement doctrine — `insp_roe`/`insp_flak`/`insp_missile`/`insp_capfloor`
behind the inspector DOCTRINE tab's Engagement chip rows, action grammar `"doc:roe:N"` /
`"doc:flak:N"` / `"doc:mp:N"` / `"doc:capfloor:N"` — and `bs_rml_roster_chip chip[4]` becomes
`chip[5]`: the 5th roster chip cycles the row's ROE via `"froe:R"`. Live-verified round trip:
chip clicks moved the highlights, the action log announced each change, and the roster chip
label tracked the state — see FleetControl). Previously 2026-08-15 (`bs_rml_hud_state` gains the fleet
panel's speed-limit gear chips — `speed_sel` + `speed_lim[5][12]` u/s labels, action grammar
`"spd:N"`; the gear list is hull-card data, see ShipCombatModel). Previously 2026-08-13 (the
ship context menu gains
`ship_menu_can_escort` — the Escort row that absorbed the retired command overlay's RMB-escort
gesture. Previously 2026-08-12: `bs_rml_hud_state` gains the fleet-ship
context menu block — `ship_menu_visible/left/top/name` + `ship_menu_can_pilot/_can_release`,
mirroring the station menu's shape — and `fleet_mode_visible` for the now release-only fleet
panel button. Earlier the same day, the inspector became a single-window UI:
`bs_rml_insp_ship` fleet-list rows + `insp_show_loadout`/`_doctrine` tab flags join the
snapshot, action grammar gains `"insp_tab:N"`; the reserved texture names "bs:portrait" and
"bs:thumbs" resolve to the engine's live offscreen targets. Earlier the same day: `insp_status`,
`bs_rml_roster_row::action_insp`, `"insp:N"`; previously the fleet-roster rows —
`bs_rml_roster_row`/`_chip`, `BS_RML_ROSTER_MAX`, `"fsel:N"`, `"fstance:N:S"`)
