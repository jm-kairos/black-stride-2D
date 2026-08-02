#pragma once

#include "defines.h"

// =====================================================================================
// RmlUi facade — the SDL-free, RmlUi-free public surface the rest of the engine and the game
// link against. Mirrors bs_imgui.h exactly in spirit: the implementation lives INSIDE the SDL
// GPU backend translation unit (renderer_backend_sdlgpu.cpp), the only TU permitted to include
// <SDL3/...> and the RmlUi headers. RmlUi is compiled into engine.dll with RMLUI_STATIC_LIB, so
// its symbols are internal to the DLL; nothing here exposes an SDL or RmlUi type, and the game
// (sandbox.exe) can only reach RmlUi through these exported bs_rml_* entry points.
//
// Division of responsibility (RmlUi = in-game UI, ImGui = editor/dev tools):
//   * bs_rml_initialize / bs_rml_shutdown are driven by renderer.cpp, bracketing bs_imgui the
//     same way (init AFTER the GPU device + window exist; shutdown BEFORE the device is destroyed).
//   * The per-frame Render call is NOT exposed here — it is sequenced internally by the backend's
//     end_frame, drawn into the swapchain pass BENEATH the ImGui editor overlay.
//   * bs_rml_update advances the context (layout, animations, hover) and syncs the context size to
//     the swapchain; call it once per frame before rendering.
//   * bs_rml_process_event feeds every SDL event to the RmlUi context (mouse/keyboard/text).
//   * bs_rml_wants_mouse / _keyboard let the game suppress world input while the cursor is over a
//     panel or a text field has focus (combine with bs_imgui_wants_* for the editor).
// =====================================================================================

// Opaque handle to a loaded RmlUi document (wraps Rml::ElementDocument*). ptr == NULL is invalid.
typedef struct bs_rml_document
{
    void* ptr;
} bs_rml_document;

// Create the RmlUi context and install the SDL_GPU render interface + system interface. Must be
// called AFTER the renderer backend has created the GPU device and claimed the window. Returns
// FALSE if RmlUi or the default (FreeType) font engine fails to initialize.
bs__api__ b8 bs_rml_initialize(void);

// Destroy the RmlUi context and release all GPU resources it owns. Must be called BEFORE the
// renderer backend destroys the GPU device. No-op if RmlUi never initialized.
bs__api__ void bs_rml_shutdown(void);

// Load a fonts directory (all .ttf/.otf files) into the RmlUi font engine. Call once after
// initialize, before loading any document that renders text. Returns the number of faces loaded.
bs__api__ i32 bs_rml_load_fonts(const char* fonts_dir);

// Load and instance an .rml document. The document starts hidden; call bs_rml_show to display it.
// Returns a handle whose ptr is NULL on failure.
bs__api__ bs_rml_document bs_rml_load_document(const char* rml_path);

// Show or hide a previously loaded document. Showing pulls it to the front of the context.
bs__api__ void bs_rml_show(bs_rml_document doc, b8 show);

// Unload and destroy a document. The handle is invalid afterwards.
bs__api__ void bs_rml_unload_document(bs_rml_document doc);

// Advance the context by one frame: sync its dimensions to the current swapchain size, then run
// layout/animation/hover updates. Call once per frame from the game loop before end_frame renders.
bs__api__ void bs_rml_update(void);

// Feed one platform event to the RmlUi context. `sdl_event` is an opaque SDL_Event*; the backend
// casts it back. Safe to call before initialization (no-op until the context exists).
bs__api__ void bs_rml_process_event(const void* sdl_event);

// TRUE when the cursor is currently over an interactive RmlUi element (last mouse-move hit a
// non-passthrough element). Gate world picking/firing on the negation, ORed with bs_imgui_wants_mouse.
bs__api__ b8 bs_rml_wants_mouse(void);

// TRUE when a RmlUi text-entry element currently holds focus and should receive keystrokes.
bs__api__ b8 bs_rml_wants_keyboard(void);

// Notify RmlUi that the drawable size changed (called from renderer_on_resize). No-op if inactive.
bs__api__ void bs_rml_on_resize(u16 width, u16 height);

// Toggle the RmlUi in-UI debugger/inspector (element tree, live RCSS, event log). A dev tool,
// wired to F12 in the game. The debugger is initialised at bs_rml_initialize; this just flips its
// visibility. No-op if RmlUi is inactive.
bs__api__ void bs_rml_debugger_toggle(void);

// Set the CAS-lite sharpening amount for FILE-LOADED UI textures (the 2x skin/icon atlases,
// minified at display). 0 disables; clamped to [0, 1]; default 0.4. Font glyphs and generated
// gradients are never sharpened (see rml.frag.hlsl).
bs__api__ void bs_rml_set_sharpen(f32 amount);

// =====================================================================================
// In-game HUD data model (Phase 4).
//
// The whole in-game HUD (nav readout, ship readout, encounter modal, action log, discovery
// browser) is a SINGLE retained RmlUi document driven by one data model named "hud". Because the
// RmlUi C++ API is internal to engine.dll, the game cannot touch Rml::* directly; instead it fills
// the POD snapshot below every frame and hands it to bs_rml_hud_update, which copies it into the
// engine-side data-model-bound state and marks everything dirty. All display formatting (units,
// percentages, colours) is done GAME-SIDE so the engine stays game-agnostic: it only shows strings.
//
// Interactions (encounter buttons, the discoveries close box) do NOT mutate game state directly.
// They enqueue a short action string that the game drains with bs_rml_hud_poll_action and acts on.
// =====================================================================================

#define BS_RML_LOG_MAX     12   // most recent action-log lines carried in a snapshot
#define BS_RML_DISC_MAX    64   // most recent discovery lines carried in a snapshot
#define BS_RML_BAY_MAX     12   // flagship-inspector Modules bay tiles (6 x 2 grid, socket-padded)
#define BS_RML_GROUP_MAX   5    // fleet-ship HUD fire-group rows (SHIP_WEAPON_GROUPS game-side)
#define BS_RML_GM_COLS     8    // fire-group matrix: max weapon columns (mounted weapons)
#define BS_RML_ACTION_CAP  32   // max bytes (incl. NUL) for a polled action string

// One action-log line (already formatted, newest last).
typedef struct bs_rml_log_line
{
    char text[128];
} bs_rml_log_line;

// One discovery line: preformatted text plus an RCSS colour string ("#rrggbbaa") for its kind.
typedef struct bs_rml_disc_line
{
    char text[192];
    char color[10];
} bs_rml_disc_line;

// A fleet-panel fire-group row: `text` = "K - weapon, weapon, ...", `action` = "group:N"
// enqueued on click, `selected` = the active fire group, `empty` = no member weapons.
typedef struct bs_rml_weapon_line
{
    char text[64];
    char action[16]; // "group:N" enqueued on click
    b8   selected;   // highlighted row (the active fire group)
    b8   empty;      // no content (dim style; group rows stay clickable)
} bs_rml_weapon_line;

// One Modules-bay tile (flagship inspector): a draggable item (weapon / point-defense /
// ship module) or an inert empty socket. `icon` names a ui-icons emblem sprite (falls back
// to the `glyph` text); `action` is enqueued on dragstart and routes the drop game-side
// ("inv:K" / "defdrag" / "mod:K"; "" = inert socket). The card_* fields are the hover
// stat card, all pre-formatted game-side; "" hides the optional blocks (mode B, footer).
typedef struct bs_rml_bay_line
{
    char glyph[8];              // fallback text symbol when `icon` is empty
    char icon[16];              // ui-icons sprite name ("ic-cannon"; "" = use glyph text)
    char action[16];            // dragstart source; "" = inert empty socket
    b8   selected;              // highlighted tile (active weapon selection)
    b8   empty;                 // empty socket (recessed style, not draggable)
    char card_title[48];        // "AUTOCANNON MK I"
    char card_rows[384];        // full mono stat sheet, '\n'-separated rows (pre)
    char card_size[64];         // "SIZE  Small - fits S/M/L mounts" (own element: colored)
    b8   card_size_ok;          // TRUE = fits a flagship hardpoint (green) / FALSE = red
    char card_desc[192];        // role description paragraph ("" hides)
    char card_mode_a[64];       // active/primary mode line ("> AP SHELLS - anti-ship")
    char card_mode_a_stats[64]; // its stat line ("DMG 6  RATE 12.0/s  PWR 1.5")
    char card_mode_b[64];       // secondary mode ("FLAK SCREEN - anti-ordnance"; "" = single-role)
    char card_mode_b_stats[64]; // its stat line
    char card_foot[80];         // keybind hint ("[T] switches fire mode..."; "" = none)
} bs_rml_bay_line;

// One cell of the flagship-inspector fire-group matrix: `action` ("gm:W:G") is enqueued on
// click to toggle weapon column W's membership in group row G; `on` renders the checkmark.
typedef struct bs_rml_gm_cell
{
    char action[16];
    b8   on;
} bs_rml_gm_cell;

// One row of the fire-group matrix (a weapon group): `label` is the group digit, `selected`
// marks the active fire group, and cell[0..col_count-1] are the per-weapon checkboxes.
typedef struct bs_rml_gm_row
{
    char           label[8];
    b8             selected;
    bs_rml_gm_cell cell[BS_RML_GM_COLS];
} bs_rml_gm_row;

// Full per-frame HUD snapshot. Zero-initialize, fill the visible sections, then push it.
typedef struct bs_rml_hud_state
{
    // Navigation readout (top-right).
    b8   nav_visible;
    char nav_sector[32];
    char nav_system[64];
    char nav_distance[32];
    char nav_zone[16];

    // Ship readout (top-right, below nav).
    b8   ship_visible;
    char ship_speed[32];

    // Time control (top-center): pause + speed tiers on the shared in-game calendar.
    b8   time_visible;
    char time_date[32];       // "Year N, Day D"
    i32  time_tier;           // active tier: 0=Pause,1=1x,2=3x,3=5x,4=10x (drives button highlight)

    // Encounter modal (centered).
    b8   enc_visible;
    char enc_name[64];
    char enc_distance[32];
    char enc_faction[64];
    char enc_desc[256];

    // Action log (bottom-right). log_alpha is the whole-panel opacity (0..1) for the idle fade.
    b8              log_visible;
    f32             log_alpha;
    i32             log_count;             // valid entries in log[0..log_count-1], newest last
    bs_rml_log_line log[BS_RML_LOG_MAX];

    // Discovery browser (floating). disc_count_label is the "%d objects logged" caption.
    b8               disc_visible;
    char             disc_count_label[32];
    i32              disc_count;           // valid entries in disc[0..disc_count-1], newest first
    bs_rml_disc_line disc[BS_RML_DISC_MAX];

    // Fleet ship panel (top-right, piloted-ship combat readout + controls).
    b8                 fleet_visible;
    char               fleet_name[64];
    char               fleet_faction[64];
    char               fleet_speed[32];
    char               fleet_heading[32];
    char               fleet_health[32];
    // Fire-group rows (always BS_RML_GROUP_MAX): text = "K - weapon, weapon, ...", selected =
    // active fire group, empty = no member weapons (dim style), action = "group:N" (select on
    // click).
    bs_rml_weapon_line fleet_group[BS_RML_GROUP_MAX];
    char               fleet_mode_label[32];  // "Pilot unit" / "Auto-pilot / RTS"
    b8                 fleet_mode_enabled;     // FALSE mid camera transition (button dimmed)
    // Capacitor + point-defense readout (Phase E feedback; docs/POINT_DEFENSE_AND_MISSILES.md).
    char               fleet_cap_w[16];       // capacitor gauge fill width ("62%")
    char               fleet_cap_label[32];   // "Capacitor 62 / 100"
    char               fleet_pd_label[40];    // "PD: OVERDRIVE - missiles first - 80%"
    b8                 fleet_pd_warn;         // TRUE when stance != STANDARD (amber cue)

    // Jump-mode banner (bottom-center; "Currently in Jump Mode").
    b8 jump_visible;

    // HierPos2 debug readout (top-left, dev toggle). debug_text may contain '\n' line breaks.
    b8   debug_visible;
    char debug_text[192];

    // Galaxy-map hover tooltip (cursor-anchored). tip_text may contain '\n' line breaks; tip_left
    // and tip_top are prebuilt CSS pixel strings ("NNNpx") bound to the element's left/top.
    b8   tip_visible;
    char tip_left[16];
    char tip_top[16];
    char tip_text[1024];

    // Flagship inspector: a persistent window opened via the bottom-center "Inspector" button.
    // inspector_btn_visible shows the launcher button during gameplay; inspector_visible shows the
    // window while it is open. The single MODULES tab lists the flagship's UNMOUNTED inventory as
    // ONE unified bay grid: weapons ("inv:K"), the point-defense ("defdrag") and ship modules
    // ("mod:K") together, padded to BS_RML_BAY_MAX with inert empty sockets. The bay well is a
    // single unmount drop target ("baydrop"; the game routes by dragged kind). Mounted items live
    // on the SHIP itself (world hardpoint boxes), not in the window; an item is shown in EITHER
    // the bay OR on a hardpoint, never both.
    b8                 inspector_btn_visible;
    b8                 inspector_visible;
    char               insp_ship_name[64];
    i32                bay_count;              // valid tiles in bay[0..count-1]
    bs_rml_bay_line    bay[BS_RML_BAY_MAX];

    // Fire-group assignment matrix (inspector): columns = mounted weapons, rows = groups 1..5.
    // gm_col_count is the number of weapon columns this frame (0 hides the section); gm_col
    // holds the short column header labels; gm_row the BS_RML_GROUP_MAX checkbox rows.
    i32                gm_col_count;
    char               gm_col[BS_RML_GM_COLS][12];
    bs_rml_gm_row      gm_row[BS_RML_GROUP_MAX];

    // Active UI font kit (0 = Neon, 1 = Clean, 2 = Minimal, 3 = Frontier). Toggles a body class
    // on the HUD document so RCSS can swap the whole document's typefaces live from the editor.
    i32                ui_kit;

    // Flagship point-defense doctrine (inspector "Point defense" section). pd_visible shows the
    // section; the three indices drive the active chip highlight and are round-tripped via the
    // actions "pd:stance:N" / "pd:pri:N" / "pd:gate:N" (see docs/POINT_DEFENSE_AND_MISSILES.md).
    b8                 pd_visible;
    i32                pd_stance;    // 0 = HOLD, 1 = STANDARD, 2 = OVERDRIVE
    i32                pd_priority;  // 0 = impact time, 1 = missiles first, 2 = nearest
    i32                pd_gate;      // 0 = 60%, 1 = 80%, 2 = 100% of resolved PD range

    // Space-station interaction: a cursor-anchored right-click context menu ("Inspect") and a
    // fullscreen Inspect window (collapsible). menu_left/top are prebuilt "NNNpx" CSS strings.
    b8                 station_menu_visible;
    char               station_menu_left[16];
    char               station_menu_top[16];
    b8                 station_inspector_visible;
    char               station_insp_title[96];
    char               station_insp_subtitle[128];  // owner civ + market specialization tag line
    b8                 station_insp_show_dock;      // true when tab 0 active
    b8                 station_insp_show_market;    // true when tab 1 active
    b8                 station_insp_show_contracts; // true when tab 2 active
    b8                 station_insp_tab2_visible;   // true when station has a Contracts room
    char               station_insp_dock[512];      // docked ships list (\n-separated, pre-formatted)
    char               station_insp_market_head[64];  // Market tab: column header line (pre-formatted)
    b8                 station_insp_spec_agri;      // true when station specializes in Agriculture
    b8                 station_insp_spec_mine;      // true when station specializes in Minerals
    b8                 station_insp_spec_vola;      // true when station specializes in Volatiles
    b8                 station_insp_spec_indu;      // true when station specializes in Industrial
    char               station_insp_market_agri[224]; // Market tab: Agriculture lines (pre-formatted)
    char               station_insp_market_mine[224]; // Market tab: Minerals lines
    char               station_insp_market_vola[224]; // Market tab: Volatiles lines
    char               station_insp_market_indu[224]; // Market tab: Industrial lines
    char               station_insp_market_note[96];  // Market tab: dim footer (station revenue)
    char               station_insp_contracts[1024]; // mission contracts list (\n-separated, pre-formatted)

    // Planet inspector: a floating window opened by left-clicking a planet on the galaxy map.
    // All text is pre-formatted GAME-SIDE (values + qualitative labels); *_w fields are CSS width
    // strings ("NN%") binding the habitability/hazard gauge bar fills.
    b8   planet_insp_visible;
    char planet_insp_title[96];      // "Veyra III"
    char planet_insp_subtitle[96];   // "Temperate Terran World" + trait tags
    char planet_insp_stats[768];     // orbit/mass/radius/temperature/gravity + environment lines
    char planet_insp_comp[256];      // resources + bulk composition survey lines
    char planet_insp_moons[384];     // moon list ("" hides the section)
    char planet_insp_events[768];    // chronicle lines ("" hides the section)
    char planet_insp_hab_w[16];      // habitability gauge fill width ("62%")
    char planet_insp_haz_w[16];      // hazard gauge fill width
    char planet_insp_hab_label[32];  // "Habitability 62%"
    char planet_insp_haz_label[32];  // "Hazard: Moderate"
} bs_rml_hud_state;

// Create the "hud" data model, load the HUD document at `rml_path`, and show it. Call once after
// bs_rml_load_fonts. Returns FALSE if the model or document could not be created.
bs__api__ b8 bs_rml_hud_init(const char* rml_path);

// Tear down the HUD document and data model. Safe to call if never initialized.
bs__api__ void bs_rml_hud_shutdown(void);

// Push a fresh HUD snapshot: copies `state` into the bound model and dirties all variables so the
// next context update re-lays-out the document. Call once per frame from game_update.
bs__api__ void bs_rml_hud_update(const bs_rml_hud_state* state);

// Dequeue the next pending UI action (from an encounter button / discoveries close). Writes a NUL-
// terminated string into `out_action` (capacity `cap`) and returns its length, or 0 if the queue is
// empty. Call in a loop until it returns 0.
bs__api__ i32 bs_rml_hud_poll_action(char* out_action, i32 cap);
