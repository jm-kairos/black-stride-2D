#pragma once

#include <defines.h>

#include <game_types.h>

#include <renderer/renderer_types.h>

#include "ship.h"

#include "projectile.h"

#include "star_fx.h"

#include "global_background.h"

#include "voronoi_galaxy.h"

#include "travel.h"

#include "fleet.h"

#include "rts_controls.h"

#include "out_sensor_detection_fx.h"

#include "profiler.h"

#include <containers/vector.h>

// =====================================================================================

// Black Stride prototype: art-texture ships with two view modes.

//

//   mode::global — zoomed out. Ship sprites and roof silhouettes visible.

//                  WASD flies the whole ship (Starsector-style inertial).

//   mode::system — galaxy map. The player navigates between star systems.

//

// Controls:

//   Global mode (ship flight, Starsector-style inertial):

//     W / S      — thrust forward / reverse along heading (accel / decel to max speed).

//     C          — brake: decelerate current velocity to zero.

//     Q / E      — strafe left / right.

//     A / D      — turn left / right (turn-accel ramps to max turn rate; auto-stabilizes).

//   M            — toggle between global and system mode.

//   Esc          — quit (handled by the engine).

// =====================================================================================

enum GameMode {

    MODE_GLOBAL = 0,

    MODE_SYSTEM

};

struct CelestialBody {

    bs_math::Vec2 position;

    f32           radius;

    bs_color      color;

    // Legacy fields (kept for backward compat, orbit_radius = semi_major_axis)

    f32           orbit_radius; // distance from parent body

    f32           orbit_speed;  // radians per second

    f32           orbit_angle;  // current angle (radians)

    // Orbital elements (constant after generation)

    f32           semi_major_axis;  // a

    f32           eccentricity;     // e (0 = circular)

    f32           arg_periapsis;    // orientation of ellipse major axis (radians)

    f32           mean_anomaly_0;   // initial phase (radians)

    f32           orbital_period;   // seconds per full orbit

};

struct StarSystem {

    bs_math::HierPos2 galaxy_center; // absolute galaxy position of this system's star

    CelestialBody     star;

    CelestialBody     planets[5];  // max 5 planets

    i32               planet_count; // 2–5

    f32               system_scale; // per-system distance compression

    const char*       name;          // display label above the star on the map

    f32               star_pulse_phase;   // random phase offset for core animation

    f32               corona_pulse_phase; // random phase offset for corona animation

    f32               halo_pulse_phase;   // random phase offset for halo animation

    bs_glow_params    glow[3];            // per-star glow: [0]=core, [1]=corona, [2]=halo

};

// A generic entity that appears on the galaxy map. Any world object with a Vec2 position

// can be synced here each frame, making the map system extensible without hardcoding ships.

#define MAX_MAP_ENTITIES 16

struct MapEntity {

    bs_math::HierPos2 galaxy_pos;

    bs_color          color;

    f32               radius;

    b8                has_outline; // if TRUE, draws an animated quad (reserved for player)

    const char*       name;        // display label for hover tooltip

};

// A lightweight combat entity that can be hit by projectiles.

// Ships register themselves here so the projectile system can sweep a flat array.

#define MAX_COMBAT_ENTITIES 32

#define HEAT_HISTORY_LEN 8

struct CombatEntity {

    b8            active;

    bs_math::HierPos2 position;

    bs_math::Vec2 render_pos; // TRANSIENT: render-space position, recomputed each frame

    bs_math::Vec2 velocity; // world velocity, for aim prediction / hit detection

    f32           radius;

    VesselFaction faction;

    f32           hp;

    Ship*         ship;   // NULL for non-ship targets; provides visual + physics

    bs_color      tint;   // fallback colour when ship == NULL

    bs_glow_params glow;  // per-entity bloom/glow override

    f32           radiation_emission; // 0..1 heat-source strength; 0 means invisible to detector

    b8            is_drone; // TRUE for SHIP_TYPE_DRONE fleet ships; FALSE for raider / non-ship targets

    bs_math::HierPos2 heat_history[HEAT_HISTORY_LEN]; // recent world positions for the heat trail

    i32           heat_history_count;             // valid entries (0..HEAT_HISTORY_LEN)

};

// ShipFlight (global-mode inertial flight dynamics) is defined in fleet.h.

// =====================================================================================

// Edit mode: click-to-select, drag-to-reposition ships and lights.

// Toggled from the EDITOR PANEL. Dragging mutates the entity's live world-space position

// (light.position or ship.origin) directly, so changes persist once edit mode is exited --

// the entity state IS the game state, no separate save step is needed.

// =====================================================================================

// What kind of entity is currently selected in edit mode.

enum EditEntityKind {

    EDIT_NONE = 0,

    EDIT_LIGHT,     // selection.index = light index into game_state::lights

    EDIT_SHIP,      // selection.index: 0 = player ship, 1 = enemy ship

};

// The editor's current selection. Always check `kind` before interpreting `index`.

struct EditSelection {

    EditEntityKind kind;

    i32            index;  // valid only when kind != EDIT_NONE

};

// How the user is currently dragging a selected entity.

enum EditDragMode : int {

    EDIT_DRAG_NONE = 0,

    EDIT_DRAG_FREE,     // unconstrained translation (existing behaviour)

    EDIT_DRAG_AXIS_X,   // translating along the X axis

    EDIT_DRAG_AXIS_Y,   // translating along the Y axis

    EDIT_DRAG_ROTATE,   // rotating via the ring

};

// Drag-in-progress state. `active` is TRUE only between mouse-down and mouse-up on a

// selected entity. Anchors capture the world-space cursor and entity positions at the

// instant the drag began, so the entity tracks the cursor without snapping.

struct EditorDrag {

    b8            active;         // TRUE while the left button is held on a selected entity

    EditDragMode  mode;           // which drag mode is active this frame

    bs_math::HierPos2 drag_anchor;    // world-space cursor position at mouse-down

    bs_math::HierPos2 entity_anchor;  // entity position at mouse-down (light.position or ship.origin)

    f32           entity_angle;   // entity angle at mouse-down (ships only, radians)

};

// Forward declaration: defined in global_background.h (included by game.cpp).

struct GlobalBackground;

struct game_state {

    u16 fb_width;

    u16 fb_height;

    // ===== CameraState — named sub-struct (consolidated: smooth zoom + free camera +
    // floating-origin system panning), accessed as s->camera_state.camera etc.
    struct CameraState {

    Camera2D  camera;          // persistent; zoom mutated by the wheel

    f32       target_zoom;      // wheel sets this; camera.zoom eases toward it (smooth zoom)

    f32       zoom_smooth_rate; // 1/s exponential ease rate for zoom (higher = snappier); editor slider

    // Free camera mode in MODE_GLOBAL: when TRUE, the camera is detached from the ship
    // and can be panned with WASD/middle-mouse. The ship continues to coast under physics.
    b8         free_camera_active;
    bs_math::HierPos2 free_camera_pos;

    // Floating-origin camera for galaxy map (MODE_SYSTEM).
    // Tracks which galaxy cell we're viewing from; camera_local is the pan offset.
    bs_math::HierPos2 camera_hierpos;
    // System-view middle-mouse drag anchors (screen-space panning).
    bs_math::Vec2 system_drag_cam;    // camera position when drag started
    bs_math::Vec2 system_drag_world;  // screen pixel position when drag started
    // Tunable system-view zoom (editor-adjustable; artists pick the best scale).
    f32           system_zoom;

    } camera_state; // end CameraState

    // ===== FleetState — named sub-struct, accessed as s->fleet_state.fleet etc.
    struct FleetState {

    Fleet     fleet;           // player ships; member 0 is the flagship (loaded from assets/ship_deck.ship)

    Ship      enemy_ship;      // hostile hull (assets/enemy_ship.ship); combat prototype

    f32       enemy_orbit_phase; // hardcoded demo orbit angle around the player flagship

    } fleet_state; // end FleetState

    // Convenience accessors for the flagship (the historical single "player ship").

    Ship&       player_ship()        { return fleet_state.fleet.flagship().ship; }

    const Ship& player_ship()  const { return fleet_state.fleet.at(0).ship; }

    ShipFlight& player_flight()       { return fleet_state.fleet.flagship().flight; }

    // ================= RenderState (lighting, bloom, nebula, starfield, star FX, backgrounds) =================
    // Named sub-struct: members accessed as s->render.lights, s->render.glow_params, etc.
    struct RenderState {

    // ---- Editor-managed 2D point lights ------------------------------------------------------

    // The player spawns/removes/edits these from the EDITOR PANEL and drags them around the scene.

    // Submitted to the renderer each frame (game_render -> renderer_set_lights) and accumulated

    // per-pixel by the sprite shader over `light_ambient`. lights are stored in WORLD space.

    Vector(bs_light2d) lights;

    bs_color      light_ambient;     // scene-global ambient floor for lighting

    i32           light_selected;    // index of the selected/edited light (-1 = none)

    // ---- Tunable shader glow parameters (editor panel) -------------------------------------

    bs_glow_params glow_params;      // copied to GPU each frame via renderer_set_glow_params

    // ---- HDR Bloom post-process tuning -----------------------------------------------------

    b8  bloom_enabled;

    f32 bloom_threshold;

    f32 bloom_intensity;

    b8  dynamic_bloom;          // when TRUE, bloom/glow intensity scales with ship speed

    b8  star_light_enabled;       // toggle volumetric star light in system view

    b8  bg_layer0_enabled;        // toggle far starfield layer (parallax 0.05)

    b8  bg_layer1_enabled;        // toggle mid starfield layer (parallax 0.40)

    b8  bg_layer2_enabled;        // toggle mapped system layer (parallax 1.0)

    b8  bg_nebula_enabled;        // toggle nebula/dust cloud layer (parallax 0.02)

    // Depth-based parallax for the celestial backdrop (stars/planets/orbit lines/test sprites).
    // Depth 0 = foreground (moves 1:1 with camera, like gameplay entities); depth 1 = fully locked
    // to the shared anchor on screen. Every system is parallaxed against ONE shared anchor (the
    // system under the camera) so their relative layout stays rigid at any depth, and the effect
    // fades to zero below bg_parallax_fade_zoom so ships stay glued to their systems on the map.
    // The same factors are used by the Map and Arena renderers, so ratios match with no cross-fade
    // seam. Co-located pairs share depth to avoid separation: planets sit on their orbit rings
    // (depth_planet == depth_orbit), test sprites orbit the star (depth_testsprite == depth_star).
    b8  bg_parallax_enabled;      // master toggle for celestial depth parallax

    f32 depth_star;               // system star (and test sprites)

    f32 depth_planet;             // planets

    f32 depth_orbit;              // orbit lines

    f32 depth_testsprite;         // test sprites (volumetric-light demo dots)

    // Zoom at/below which celestial parallax has fully faded to zero (stars sit at true positions
    // so ships stay glued to their systems on the map). Parallax ramps back to full over a fixed
    // smoothstep band above this value. Editable in the editor panel.
    f32 bg_parallax_fade_zoom;

    // Procedural nebula/dust tunables (editor panel).

    f32 nebula_intensity;          // global nebula opacity multiplier (default 0.5)

    f32 nebula_dust_intensity;     // dust cloud opacity multiplier (default 0.4)

    bs_color nebula_gas_color_a;   // deep base gas color

    bs_color nebula_gas_color_b;   // mid gas color

    bs_color nebula_gas_color_c;   // bright highlight core color

    bs_color nebula_dust_color;    // dark dust silhouette color

    f32 nebula_gas_brightness_mul; // extra luminance multiplier for gas (default 1.0)

    f32 nebula_highlight_power;    // how much the bright core is emphasized (default 1.0)

    f32 nebula_palette_shift;      // rotates the cosine palette index (default 0.0)

    f32 nebula_swirl_strength;     // domain rotation strength (default 1.0)

    f32 nebula_falloff_radius;     // controls radial/band fade (default 2.0)

    f32 nebula_band_strength;      // how much Milky Way band falloff is applied (default 0.5)

    f32 nebula_lod_target;         // world units per noise unit at zoom==1 (LOD feature scale, default 3500)

    f32 nebula_parallax;           // scroll parallax 0..1 (1 == world-locked, <1 == distant/slower, default 0.08)

    // Galaxy-wide "biome" variation: a low-frequency macro field modulates nebula properties across
    // the galaxy so different regions look distinct. biome_strength==0 reproduces the uniform look.

    f32 nebula_biome_strength;     // 0..1 how strongly biomes recolor/reshape the nebula (default 0.6)

    f32 nebula_biome_scale;        // world units per biome noise unit (region size, default 250000)

    f32 nebula_biome_hue_spread;   // 0..1 how far the color family rotates between biomes (default 0.5)

    f32 nebula_zoom_detail;        // 0..1 extra fine filament detail emerging as you zoom in (default 0.6)

    f32 nebula_zoom_saturation;    // 0..1 extra saturation/contrast as you zoom in (default 0.5)

    // Procedural starfield tunables (editor panel).

    f32 starfield_lod_density;     // cell fill rate 0..1 (default 0.06)

    f32 starfield_lod_size;        // star size multiplier (default 1.0)

    f32 starfield_lod_brightness;  // overall brightness multiplier (default 1.0)

    // Virtual-quadtree LOD controls (feed the starfield shader's level selection).

    f32 starfield_lod_target_px;   // desired on-screen cell size in px for LOD peak (default 40)

    f32 starfield_lod_levels;      // number of LOD slots accumulated per pixel (default 4)

    f32 starfield_lod_factor;      // cell-size ratio between LOD levels (quadtree = 4, default 4)

    f32 starfield_parallax_near;   // finest-level scroll parallax 0..1 (1 == world-locked, default 0.5)

    f32 starfield_parallax_falloff;// per-level slowdown ratio; coarser levels move slower (default 0.75)

    // Star dazzle effect: fade starfield near the bright central star.
    // NOTE: star_pos lives in the per-frame RenderContext block below (transient render scratch).

    f32 star_dazzle_inner_radius;  // world units: fully suppressed inside this

    f32 star_dazzle_outer_radius;  // world units: no suppression outside this

    f32 star_dazzle_intensity;     // 0..1 suppression strength

    f32 star_light_intensity_mul; // multiplier on star light intensity (default 1.0)

    f32 star_light_radius_mul;    // multiplier on star light radius (default 1.0)

    bs_texture exhaust_texture;   // soft radial gradient for engine exhaust (runtime-generated)

    bs_texture emblem_drone;      // combat-mode ship type emblem (loaded from PNG)

    bs_texture emblem_extractor;  // combat-mode ship type emblem (loaded from PNG)

    bs_glow_params exhaust_glow;  // per-entity glow for engine exhaust

    bs_glow_params bullet_glow;   // per-entity glow for combat entities (bullets, etc.)

    StarFxSystem  star_fx;            // owns textures + draw logic for star visuals

    GlobalBackground global_background; // parallax background layers for MODE_GLOBAL

    } render; // ================= end RenderState =================

    // ===== EditorState — named sub-struct, accessed as s->editor.edit_mode_active etc.
    struct EditorState {

    // ---- Edit mode: click to select, drag to reposition ships and lights -------------------

    b8            edit_mode_active;  // toggled from the EDITOR PANEL

    EditSelection edit_selection;    // what is currently selected

    EditorDrag    edit_drag;         // drag-in-progress state

    } editor; // end EditorState

    // ===== ViewState — named sub-struct, accessed as s->view.mode etc.
    struct ViewState {

    GameMode  mode;            // current view/control mode

    b8         alt_movement_active; // TRUE while SHIFT-toggled mouse-follow flight mode is active

    } view; // end ViewState

    // ===== RenderContext: PER-FRAME transient render scratch — recomputed every frame in
    // game_render / the render passes. NOT persistent game state; must never be saved/loaded or
    // persisted across frames. Anonymous so members stay accessible as s->view_arena_w, s->star_pos.
    struct {

    // STEP 2 (view fusion): continuous arena<->galaxy-map blend weight for the current frame,
    // derived from zoom (1.0 = arena/gameplay look, 0.0 = galaxy-map look). Recomputed every frame
    // in game_render; every render site cross-fades on this instead of branching on `mode`.
    f32        view_arena_w;

    // Shared parallax anchor for the celestial backdrop this frame: the galaxy_center of the
    // system under the camera. ONE anchor is used for every system so their relative layout stays
    // rigid under depth parallax (a per-system anchor collapses all systems to screen center as
    // depth->1). Transient; recomputed every frame in game_render. Never persist.
    bs_math::HierPos2 celestial_anchor;

    // Current star world position (render-space offset from camera center): set by the parallax
    // background pass and the ship pass, read by ship_scene + starfield. Per-frame only.
    bs_math::Vec2 star_pos;

    }; // end RenderContext

    // NOTE: free-camera fields (free_camera_active/free_camera_pos) are now part of the
    // consolidated CameraState sub-struct near the top of game_state.

    // ---- RTS controls (selection, orders, control groups) ----------------------------------

    RtsControls rts_controls;

    // ---- Editor-gated continuous travel (fork #1 prototype) ----------------------------------

    b8            travel_enabled; // EDITOR PANEL: master toggle for travel system

    b8            travel_paused;  // EDITOR PANEL: pause travel mid-flight

    TravelState   travel;         // hierarchical-precision travel state

    // ---- Galaxy cluster (multiple star systems) --------------------------------------------

#define GALAXY_MAX_SYSTEMS 64

    // ===== GalaxyState — named sub-struct (consolidated: systems + map entities +
    // map marker/anim/ranges + recenter animation), accessed as s->galaxy.systems etc.
    struct GalaxyState {

    StarSystem    systems[GALAXY_MAX_SYSTEMS];

    i32           system_count;       // number of populated entries

    GalaxyVoronoi galaxy_voronoi;      // Voronoi diagram for system territories

    i32           current_system;     // cached nearest-system index (-1 = deep space)

    // Generic galaxy-map entities (ships, stations, asteroids, etc.).

    // Rebuilt each frame from world-space positions via world_to_galaxy_pos().

    MapEntity     map_entities[MAX_MAP_ENTITIES];

    i32           map_entity_count;

    // ---- Galaxy map player marker animation --------------------------------------------
    f32  galaxy_map_time;
    bool map_anim_scale;
    bool map_anim_rotate;
    bool map_anim_alpha;
    bool map_anim_thickness;
    // Hyperjump range circle (editor-tunable, shown on galaxy map)
    bool map_draw_jump_range;
    f32  map_jump_range;
    // Sensor detection range (editor-tunable, shown on galaxy map)
    bool map_draw_sensor_range;
    f32  map_sensor_range;
    // Delaunay lane connections between star systems (galaxy map)
    bool map_draw_lanes;

    // Map recenter animation (P key in MODE_SYSTEM)
    bool map_recentering;
    f32  map_recenter_t;
    bs_math::HierPos2 map_recenter_from_pos;
    f32  map_recenter_from_zoom;
    bs_math::HierPos2 map_recenter_target_pos;
    f32  map_input_cooldown; // seconds until pan/drag re-enabled after recenter
    bool map_drag_needs_fresh_press; // TRUE after recenter until middle mouse released

    } galaxy; // end GalaxyState

    // NOTE: floating-origin + system-panning fields (camera_hierpos, system_drag_cam,
    // system_drag_world, system_zoom) are now part of the consolidated CameraState sub-struct
    // near the top of game_state.

    // NOTE: galaxy-map marker/anim/range fields (galaxy_map_time, map_anim_*, map_draw_*,
    // map_jump_range, map_sensor_range) are now part of the consolidated GalaxyState sub-struct.

    // ---- Dedicated ship sensor (visual contact range in global/combat mode) ---------------

    f32 ship_sensor_range;        // radius within which the enemy ship renders as itself

    OutSensorDetectionFX out_sensor_fx; // out-of-sensor-range interference effect

    // ---- Radiation detector / Metaball movement UI (EDITOR PANEL controlled) ------------

    b8  show_metaball_ui;

    f32 base_detection_radius;    // radius of the detector field around each player ship
    f32 heat_signature_radius;    // visual radius of the heat signature blob around emitters; independent of detection range

    // ---- Heat map gradient controls ------------------------------------------------------
    i32      heat_palette;             // bs_heat_palette index
    bs_color heat_color_low;           // custom edge color
    bs_color heat_color_high;          // custom center color
    f32      heat_color_falloff_power; // power applied to normalized value before gradient lookup

    f32 metaball_radius_factor;   // multiplier for encounter detection range

    f32 metaball_threshold;       // threshold for heat map alpha and encounter detection

    f32 heat_map_intensity;       // global opacity multiplier for the heat map overlay

    f32 heat_tail_length;         // number of active trail points (0 = no tail)

    f32 heat_tail_fade;           // falloff exponent for trail emission

    f32 heat_warp_strength;       // world units of domain warp displacement

    f32 heat_map_venn_sharpness;  // 0.0 = organic soft Venn boundary, 1.0 = hard-edged sensor circle

    // ---- Performance timing (EDITOR PANEL readouts) -----------------------------------------

    f64 perf_frame_start_ns;      // std::chrono nanoseconds at the start of the frame

    f64 perf_heat_map_start_ns;   // std::chrono nanoseconds at start of heat map work

    f32 heat_map_cpu_ms;          // rolling average heat map CPU time in milliseconds

    f32 frame_ms;                 // rolling average frame time in milliseconds

    Profiler profiler;            // per-subsystem CPU profiler (PROFILER panel readout)

    // ---- Time control (RTS-style pause/resume, extensible to speed-up) --------------------

    f32 time_scale;         // 0.0 = paused, 1.0 = normal, 2.0 = 2×, etc.

    f32 elapsed_time;       // monotonic seconds since game_init (for procedural FX)

    // ---- Encounter system (blob merge → pause → choose action) ---------------------------

    b8  encounter_active;       // TRUE while encounter panel is showing

    b8  encounter_was_active;   // previous-frame value for edge detection

    b8  encounter_can_retrigger;// TRUE when ships have separated after last encounter

    // ---- Action Log (player-activity HUD) ----------------------------------------------

    // Rolling buffer of up to 30 significant player actions. Fades after 3s inactivity,

    // expands to full history on hover.

    struct {

        char entries[30][128]; // rolling message buffer (oldest at 0)

        i32  count;             // valid entries (0..30)

        f32  inactivity_timer;  // seconds since last push

    } action_log;

    // NOTE: map recenter-animation fields (map_recentering, map_recenter_*, map_input_cooldown,
    // map_drag_needs_fresh_press) are now part of the consolidated GalaxyState sub-struct.

    // ---- Projectile system -------------------------------------------------------------

    ProjectileSystem projectiles;

    // ---- Combat entities ---------------------------------------------------------------

    CombatEntity combat_entities[MAX_COMBAT_ENTITIES];

    i32          combat_entity_count;

};

// Shared constants / helpers used by parallax layer code (mapped_system_layer.cpp).

extern const f32 STAR_MIN_SCREEN_RADIUS;

extern const f32 STAR_DIST_SCALE_FACTOR;

extern const f32 STAR_MAX_DIST_SCALE;

extern const f32 STAR_HERO_MAP_MIN_RADIUS;

extern f32 sensor_visibility_from_dist(f32 dist, f32 range);

// Sensor visibility (0..1) for an entity at render-local position (galaxy-map look + lighting).
extern f32 get_sensor_visibility(const game_state* s, bs_math::Vec2 pos);

// Ship physics constants (shared with RTS autopilot in rts_controls.cpp).

extern const f32 SHIP_ACCEL;

extern const f32 SHIP_DECEL;

extern const f32 SHIP_MAX_SPEED;

extern const f32 SHIP_TURN_ACCEL;

extern const f32 SHIP_MAX_TURN;

// NOTE: the module-include cascade that used to live here now lives in game_modules.h, which is
// included ONLY by game.cpp. Modules must include the specific peer headers they call (they get
// the game_state definition from game.h). This keeps a single module-header change from
// recompiling every game.h includer and makes real peer dependencies explicit.

b8 game_init(Game* game_inst);

b8 game_update(Game* game_inst, f32 dt);

b8 game_render(Game* game_inst, f32 dt);

void game_on_resize(Game* game_inst, u32 width, u32 height);

