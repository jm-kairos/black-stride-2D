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
struct CombatEntity {
    b8            active;
    bs_math::Vec2 position;
    bs_math::Vec2 velocity; // world velocity, for aim prediction / hit detection
    f32           radius;
    VesselFaction faction;
    f32           hp;
    Ship*         ship;   // NULL for non-ship targets; provides visual + physics
    bs_color      tint;   // fallback colour when ship == NULL
    bs_glow_params glow;  // per-entity bloom/glow override
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
enum EditDragMode {
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
    bs_math::Vec2 drag_anchor;    // world-space cursor position at mouse-down
    bs_math::Vec2 entity_anchor;  // entity position at mouse-down (light.position or ship.origin)
    f32           entity_angle;   // entity angle at mouse-down (ships only, radians)
};
// Forward declaration: defined in global_background.h (included by game.cpp).
struct GlobalBackground;
struct game_state {
    u16 fb_width;
    u16 fb_height;
    Camera2D  camera;          // persistent; zoom mutated by the wheel
    Fleet     fleet;           // player ships; member 0 is the flagship (loaded from assets/ship_deck.ship)
    Ship      enemy_ship;      // hostile hull (assets/enemy_ship.ship); combat prototype
    // Convenience accessors for the flagship (the historical single "player ship").
    Ship&       player_ship()        { return fleet.flagship().ship; }
    const Ship& player_ship()  const { return fleet.at(0).ship; }
    ShipFlight& player_flight()       { return fleet.flagship().flight; }
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
    // Procedural starfield tunables (editor panel).
    f32 starfield_lod_density;     // cell fill rate 0..1 (default 0.06)
    f32 starfield_lod_size;        // star size multiplier (default 1.0)
    f32 starfield_lod_brightness;  // overall brightness multiplier (default 1.0)
    // Star dazzle effect: fade starfield near the bright central star.
    bs_math::Vec2 star_pos;        // current star world position (updated per frame)
    f32 star_dazzle_inner_radius;  // world units: fully suppressed inside this
    f32 star_dazzle_outer_radius;  // world units: no suppression outside this
    f32 star_dazzle_intensity;     // 0..1 suppression strength
    f32 star_light_intensity_mul; // multiplier on star light intensity (default 1.0)
    f32 star_light_radius_mul;    // multiplier on star light radius (default 1.0)
    bs_texture exhaust_texture;   // soft radial gradient for engine exhaust (runtime-generated)
    bs_glow_params exhaust_glow;  // per-entity glow for engine exhaust
    bs_glow_params bullet_glow;   // per-entity glow for combat entities (bullets, etc.)
    StarFxSystem  star_fx;            // owns textures + draw logic for star visuals
    GlobalBackground global_background; // parallax background layers for MODE_GLOBAL
    // ---- Edit mode: click to select, drag to reposition ships and lights -------------------
    b8            edit_mode_active;  // toggled from the EDITOR PANEL
    EditSelection edit_selection;    // what is currently selected
    EditorDrag    edit_drag;         // drag-in-progress state
    GameMode  mode;            // current view/control mode
    b8         alt_movement_active; // TRUE while SHIFT-toggled mouse-follow flight mode is active
    // Free camera mode in MODE_GLOBAL: when TRUE, the camera is detached from the ship
    // and can be panned with WASD/middle-mouse. The ship continues to coast under physics.
    b8         free_camera_active;
    bs_math::Vec2 free_camera_pos;
    // ---- RTS controls (selection, orders, control groups) ----------------------------------
    RtsControls rts_controls;
    // ---- Editor-gated continuous travel (fork #1 prototype) ----------------------------------
    b8            travel_enabled; // EDITOR PANEL: master toggle for travel system
    b8            travel_paused;  // EDITOR PANEL: pause travel mid-flight
    TravelState   travel;         // hierarchical-precision travel state
    // ---- Galaxy cluster (multiple star systems) --------------------------------------------
#define GALAXY_MAX_SYSTEMS 64
    StarSystem    systems[GALAXY_MAX_SYSTEMS];
    i32           system_count;       // number of populated entries
    GalaxyVoronoi galaxy_voronoi;      // Voronoi diagram for system territories
    i32           current_system;     // cached nearest-system index (-1 = deep space)
    // Generic galaxy-map entities (ships, stations, asteroids, etc.).
    // Rebuilt each frame from world-space positions via world_to_galaxy_pos().
    MapEntity     map_entities[MAX_MAP_ENTITIES];
    i32           map_entity_count;
    // Floating-origin camera for galaxy map (MODE_SYSTEM).
    // Tracks which galaxy cell we're viewing from; camera_local (below) is the pan offset.
    bs_math::HierPos2 camera_hierpos;
    // System-view middle-mouse drag anchors (screen-space panning).
    bs_math::Vec2 system_drag_cam;    // camera position when drag started
    bs_math::Vec2 system_drag_world;  // screen pixel position when drag started
    // Tunable system-view zoom (editor-adjustable; artists pick the best scale).
    f32           system_zoom;
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
    // ---- Metaball movement UI (EDITOR PANEL controlled) ----------------------------------
    b8  show_metaball_ui;
    f32 metaball_radius_factor;
    f32 metaball_threshold;
    i32 metaball_grid_w;
    i32 metaball_grid_h;
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
    // Map recenter animation (P key in MODE_SYSTEM)
    bool map_recentering;
    f32  map_recenter_t;
    bs_math::Vec2 map_recenter_from_pos;
    f32  map_recenter_from_zoom;
    bs_math::Vec2 map_recenter_target_pos;
    f32  map_input_cooldown; // seconds until pan/drag re-enabled after recenter
    bool map_drag_needs_fresh_press; // TRUE after recenter until middle mouse released
    // ---- Projectile system -------------------------------------------------------------
    ProjectileSystem projectiles;
    // ---- Combat entities ---------------------------------------------------------------
    CombatEntity combat_entities[MAX_COMBAT_ENTITIES];
    i32          combat_entity_count;
};
// Shared constants / helpers used by parallax layer code (mapped_system_layer.cpp).
extern const f32 STAR_MIN_SCREEN_RADIUS;
extern f32 sensor_visibility_from_dist(f32 dist, f32 range);
// Ship physics constants (shared with RTS autopilot in rts_controls.cpp).
extern const f32 SHIP_ACCEL;
extern const f32 SHIP_DECEL;
extern const f32 SHIP_MAX_SPEED;
extern const f32 SHIP_TURN_ACCEL;
extern const f32 SHIP_MAX_TURN;
// Galaxy coordinate helpers.
i32 find_nearest_system(const bs_math::HierPos2* ship_pos,
                        const StarSystem* systems, i32 count);
bs_math::Vec2 galaxy_to_system_local(const bs_math::HierPos2* ship_galaxy,
                                     const bs_math::HierPos2* system_center);
void action_log_push(game_state* s, const char* fmt, ...);
b8 game_init(Game* game_inst);
b8 game_update(Game* game_inst, f32 dt);
b8 game_render(Game* game_inst, f32 dt);
void game_on_resize(Game* game_inst, u32 width, u32 height);
