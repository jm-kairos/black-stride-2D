#pragma once

#include <defines.h>

#include <math/math_utils.h>

#include <math/bs_hierpos.h>

#include <renderer/renderer_types.h>

#include "render/ship_visual.h"

// =====================================================================================

// Ship definition (art-texture based).

//

// Ships are loaded from `.ship` files: a flat text format defining name, faction, visual

// sprite layers, and an authored collision polygon. No tile grid — rendering is purely

// through the decoupled ShipVisual sprite layers.

//

// Coordinate convention: the ship is a RIGID BODY with a world pose of two parts — a

// world-space `origin` (position, moved in global mode) and an `angle` heading (radians,

// CCW, turned in global mode). The visual art and collider are authored in SHIP-LOCAL

// space centered on {0,0}. The two frames relate by the full pose:

//   world = origin + rotate(local * world_scale, angle);

//   local = rotate(world - origin, -angle) / world_scale;

// =====================================================================================

#define SHIP_MAX_COLLIDER_VERTS 32

#define SHIP_MAX_WEAPONS 4

#define SHIP_MAX_HARDPOINTS 16

// =====================================================================================
// Hardpoint skeleton (Phase 1 of the ship-module system). Each hull authors a set of
// typed module slots ("boxes") in SHIP-LOCAL space — the same art-texel space the
// collider uses (1 local unit == 1 art pixel when the .ship `size` matches the PNG),
// so a hardpoint sits at a precise pixel of the sprite. Authored per hull in the .ship
// file:
//   hardpoint <id> <accepts> <size> <x> <y> <facing_deg> <arc_deg>
// where <accepts> is one or more module kinds joined by '|' (e.g. weapon|defense),
// <size> is S/M/L, <x>,<y> are ship-local coords (+Y = nose), <facing_deg> is the
// mount's rest direction CCW from the nose, and <arc_deg> is its total traverse arc.
// Phase 1 only stores + renders the skeleton; mounting modules into it is Phase 2.
// =====================================================================================

// Module kinds a hardpoint can accept (bitmask).
#define MODULE_TYPE_WEAPON  (1u << 0)
#define MODULE_TYPE_DEFENSE (1u << 1)
#define MODULE_TYPE_SENSOR  (1u << 2)
#define MODULE_TYPE_ENGINE  (1u << 3)
#define MODULE_TYPE_UTILITY (1u << 4)

enum HardpointSize {

    HARDPOINT_SMALL = 0,

    HARDPOINT_MEDIUM,

    HARDPOINT_LARGE,

};

struct HardpointDef {

    char          id[32];      // author-facing slot name ("bow_gun", "radar_dome", ...)

    u32           accepts;     // MODULE_TYPE_* bitmask of module kinds this slot takes

    HardpointSize size;        // physical slot size (S/M/L); gates module size in Phase 2

    bs_math::Vec2 pos_local;   // ship-local (art texel) position of the slot's center

    f32           facing;      // mount rest direction (radians, CCW from nose = +Y local)

    f32           arc;         // total traverse arc (radians) centered on facing

};

// Half-extent (ship-local units) of a hardpoint's visual/placement box.

f32 hardpoint_half_extent(HardpointSize s);

enum VesselFaction {

    VESSEL_PIRATE     = 0,

    VESSEL_FEDERATION = 1,

    VESSEL_NEUTRAL    = 2,

    VESSEL_STATION    = 3,

    VESSEL_DERELICT   = 4,

};

// =====================================================================================
// Feature B: unified faction id. A ship's faction is a single i16:
//   >= 0            -> galaxy civilization index (s->galaxy.civs[faction_id]); the empire it serves.
//   FACTION_PLAYER  -> the player's own fleet (never hostile to itself).
//   FACTION_PIRATE  -> lawless raiders / unclaimed wild space (always hostile).
//   FACTION_NONE    -> unset.
// Runs in parallel with the legacy VesselFaction enum (visuals / friendly-fire) during the
// migration; stance/diplomacy is resolved from faction_id via galaxy_history_faction_is_hostile.
// =====================================================================================
#define FACTION_NONE   ((i16)-1)
#define FACTION_PLAYER ((i16)-2)
#define FACTION_PIRATE ((i16)-3)

const char* vessel_faction_name(VesselFaction f);

const char* vessel_faction_desc(VesselFaction f);

// =====================================================================================

// Sensor suite (per-ship). Three concentric detection layers of decreasing radius. Potency

// attributes (accuracy / detection strength) are yet-to-be-defined and will extend this struct.

//   Layer 2 (outer) : long-range anomaly detection (e.g. radiation spikes from fired weapons).

//   Layer 1 (mid)   : identification range (a contact stops fading and can be identified).

//   Layer 0 (inner) : comfort zone; no gameplay effect yet.

// =====================================================================================

#define SENSOR_LAYER0_RADIUS 15000.0f

#define SENSOR_LAYER1_RADIUS 30000.0f

#define SENSOR_LAYER2_RADIUS 50000.0f

struct SensorSuite {

    f32 layer0_radius = SENSOR_LAYER0_RADIUS; // comfort zone

    f32 layer1_radius = SENSOR_LAYER1_RADIUS; // identification range

    f32 layer2_radius = SENSOR_LAYER2_RADIUS; // long-range detection

};

// =====================================================================================

// Defense laser (per-ship point-defense). Auto-engages the nearest HOSTILE PROJECTILE that

// enters the ship's engagement range, snapping a beam onto it and tracking its exact position

// for a short dwell while applying damage-per-second. Destroys the projectile when its HP is

// depleted; survivors are released and may be re-engaged. `range` defaults to 0, which the

// point-defense subsystem resolves to the ship's LIVE Layer 1 sensor radius each frame -- so

// changing Layer 1 changes the laser range with it.

// =====================================================================================

struct DefenseLaser {

    b8  enabled            = TRUE;    // auto-fire when a target is in range

    f32 range              = 0.0f;    // 0 => use sensors.layer1_radius (live-coupled)

    f32 damage_per_second  = 12.0f;   // damage applied to a locked projectile's HP

    f32 dwell_time         = 0.15f;   // seconds the beam tracks one target before releasing

    f32 retarget_cooldown  = 0.08f;   // brief gap before acquiring the next target

    // ---- runtime state (not tuning) ----------------------------------------------------

    i32 target_index       = -1;      // locked projectile pool slot; -1 = none

    f32 dwell_remaining    = 0.0f;

    f32 cooldown_remaining = 0.0f;

};

// =====================================================================================
// Ship size classes. Motion feel scales with hull size: a drone flits, a cruiser is a
// slow-turning wall of metal. The class is derived automatically from the hull's WORLD
// length (size_local.y * world_scale) at ship_load time, or authored explicitly in the
// .ship file with `class <drone|corvette|frigate|destroyer|cruiser|capital>`.
// =====================================================================================

enum ShipSizeClass {

    SHIP_CLASS_DRONE = 0,   // < 60 units   — nimble strike craft (legacy feel)

    SHIP_CLASS_CORVETTE,    // < 150 units

    SHIP_CLASS_FRIGATE,     // < 350 units

    SHIP_CLASS_DESTROYER,   // < 700 units

    SHIP_CLASS_CRUISER,     // < 1800 units — heavy line ship

    SHIP_CLASS_CAPITAL,     // >= 1800 units

    SHIP_CLASS_COUNT,

};

// Per-class inertial flight tuning consumed by ship_control (manual piloting) and the
// fleet autopilot (move/attack orders). Same shape as the legacy SHIP_* globals, which
// remain as the DRONE baseline.

struct ShipMotion {

    f32 max_speed;   // linear speed cap (units/s)

    f32 accel;       // forward / strafe thrust (units/s^2)

    f32 decel;       // reverse + brake thrust (units/s^2)

    f32 turn_accel;  // angular ramp / auto-stabilize rate (rad/s^2)

    f32 max_turn;    // angular speed cap (rad/s)

};

// Classify a hull by its WORLD length (size_local.y * world_scale).

ShipSizeClass ship_size_class_from_length(f32 hull_length_world);

// Motion tuning table lookup for a size class.

const ShipMotion& ship_motion_for_class(ShipSizeClass c);

// Display name ("Drone", "Cruiser", ...).

const char* ship_size_class_name(ShipSizeClass c);

struct Ship {

    f32           world_scale;    // exterior scale multiplier

    ShipSizeClass size_class;     // hull size category (drives motion feel)

    ShipMotion    motion;         // per-ship flight tuning, resolved at load from size_class

    bs_math::HierPos2 origin;    // world position (hierarchical grid cell + local offset)

    bs_math::Vec2 render_pos;    // TRANSIENT: render-space position, recomputed each frame

    f32           angle;          // world heading in radians (CCW)

    VesselFaction faction;        // who this ship belongs to (legacy enum; visuals / friendly-fire)

    i16           faction_id;     // Feature B: unified faction (civ index / FACTION_PLAYER / FACTION_PIRATE)

    const char*   vessel_name;    // display name shown in encounters / HUD

    // ---- Visual layers (sprite art, loaded from .ship file) -----------------------

    ShipVisual    visual;

    // ---- Footprint (ship-local width/height, from .ship file) ----------------------

    bs_math::Vec2 size_local;

    // ---- Authored collision polygon (ship-local space, convex, CCW) ----------------

    bs_math::Vec2 collider_verts[SHIP_MAX_COLLIDER_VERTS];

    i32           collider_count;

    // ---- Hardpoint skeleton (typed module slots, authored in .ship) ----------------

    HardpointDef  hardpoints[SHIP_MAX_HARDPOINTS];

    i32           hardpoint_count;

    // ---- Mounted modules (one per hardpoint slot) ---------------------------------------
    // mounts[i] is the Weapon mounted on hardpoints[i] (nullptr = empty). The point-defense
    // occupies a hardpoint via point_defense_mount instead (it is a DefenseLaser, not a
    // Weapon). A given hardpoint holds at most ONE of the two. Mounting is type-gated by
    // hardpoints[i].accepts (weapons need MODULE_TYPE_WEAPON, the PD MODULE_TYPE_DEFENSE).

    struct Weapon* mounts[SHIP_MAX_HARDPOINTS];

    i32            active_weapon_idx; // hardpoint index of the firing weapon; -1 = none

    // ---- Unmounted weapon stash (loadout inventory) ------------------------------------
    // Weapons the ship owns but has NOT mounted on a hardpoint. The flagship-inspector Arsenal
    // editor drags weapons between this stash (the left "available" list) and mounts[] (the
    // hardpoints on the right). mounts[] and weapon_stash[] never hold the same pointer.

    struct Weapon* weapon_stash[SHIP_MAX_WEAPONS];

    i32            weapon_stash_count; // valid entries in weapon_stash[0..count-1]

    // ---- Weapon hardpoint (ship-local, default center; authored in .ship later) ------

    bs_math::Vec2  weapon_fire_offset_local;

    // ---- Per-entity bloom/glow parameters ----------------------------------------------

    bs_glow_params glow;

    // ---- Radiation heat-source emission (0..1) ---------------------------------------

    f32           radiation_emission;

    // ---- Sensor suite (three concentric detection layers) ----------------------------

    SensorSuite   sensors;

    // ---- Point-defense laser (auto-targets incoming hostile projectiles) -------------

    DefenseLaser  point_defense;

    // Which hardpoint the point-defense currently occupies, or -1 when it is unmounted
    // (sitting in the defensive inventory). Only the flagship's inspector manages this;
    // other ships leave the point-defense always-on (mount -1).

    i32           point_defense_mount = -1;

};

// ---- Hardpoint / mount queries ---------------------------------------------------------

// TRUE when the hardpoint's accepts-mask includes the given MODULE_TYPE_* kind.
b8 hardpoint_accepts(const HardpointDef* hp, u32 module_type);

// The weapon mounted on the ship's active hardpoint, or nullptr when none is selected.
struct Weapon* ship_active_weapon(const Ship* ship);

// First hardpoint accepting `module_type` that has neither a weapon nor the point-defense
// mounted, or -1 when none is free. Used for init-time auto-mounting.
i32 ship_first_free_hardpoint(const Ship* ship, u32 module_type);

// Load a ship from a `.ship` file (path relative to the working directory). Returns FALSE

// on any parse/IO error (and logs why); on success `out_ship` is fully populated with

// origin = {0,0}. Call once during game init.

b8 ship_load(Ship* out_ship, const char* path);

// Select the n-th (0-based) mounted weapon in hardpoint order as the active weapon.
// No-op when fewer than n+1 weapons are mounted.
void ship_select_weapon_slot(Ship* ship, i32 n);

// ---- Rigid-body transforms -----------------------------------------------------------

// Ship-local direction offset: rotates + scales a ship-local vector by the pose's angle and
// scale, but does NOT add the origin. Frame-independent (small Vec2); add it to a HierPos2
// origin (sim) or a render-space base (draw) as needed.

bs_math::Vec2 ship_local_dir(const Ship* ship, bs_math::Vec2 local);

// Ship-local point -> world point: applies the full pose (origin AND angle). Returns a
// hierarchical position (precise at any distance from the galaxy origin).

bs_math::HierPos2 ship_local_to_world(const Ship* ship, bs_math::Vec2 local);

// World point -> ship-local point: removes the full pose (inverse of ship_local_to_world).

bs_math::Vec2 ship_world_to_local(const Ship* ship, bs_math::HierPos2 world);

// ---- Ship-ship collision -----------------------------------------------------------

// World-space bounding-circle radius: half the diagonal of the collider bounding box,

// so it encloses the hull at ANY heading (rotation-invariant). Cheap broad-phase bound.

f32 ship_bounding_radius(const Ship* ship);

// Broad-phase collision: TRUE if two ships' world bounding circles overlap.

b8 ships_overlap(const Ship* a, const Ship* b);

// Narrow-phase polygon collision: tests the two authored collider polygons (transformed

// to world space) with the Separating-Axis Theorem. Returns TRUE when they overlap and

// writes the Minimum Translation Vector to *out_mtv (world units): the SHORTEST displacement

// that moves `a` clear of `b`. Returns FALSE when a separating axis exists (disjoint).

b8 ships_collide(const Ship* a, const Ship* b, bs_math::Vec2* out_mtv);

// Debug/introspection: writes the collider vertices relative to the ship ORIGIN (rotated +
// scaled ship-local offsets, small Vec2 each) in winding order. Add the ship's render-space
// base (or hierpos origin) to place them. Returns FALSE if the ship has no collider (count == 0).

b8 ship_collider_corners(const Ship* ship, bs_math::Vec2 out_corners[SHIP_MAX_COLLIDER_VERTS]);

