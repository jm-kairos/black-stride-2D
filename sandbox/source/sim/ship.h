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

// Unmounted-weapon stash capacity. The flagship's stash doubles as the FLEET-WIDE pool the
// Arsenal inspector draws from, so it must hold the whole starting armory: the flagship's
// four-archetype rack PLUS one gauss per escort (escorts spawn unarmed by design).
#define SHIP_MAX_WEAPONS 8

#define SHIP_MAX_HARDPOINTS 16

#define SHIP_MAX_MODULES 4   // unmounted module rack (loadout inventory) capacity

#define SHIP_WEAPON_GROUPS 5 // player fire-control groups (number keys 1..5)

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

    // Multiplier on everything this slot DRAWS, and on where its shots leave. 1.0 = the size
    // implied by the slot class. Purely presentational: it scales the mount art, the procedural
    // turret/dish rectangles and the muzzle origins together, and touches no rule -- module fit
    // still keys off `size`, collision off the hull collider, damage off the weapon.
    //
    // A default member initialiser, not a value set at the parse site, because both
    // construction sites use `HardpointDef{}` and a zeroed scale would draw every mount at
    // nothing. Optional 8th field on a `hardpoint` line; omit it and the slot is unchanged.
    f32           art_scale = 1.0f;

};

// =====================================================================================
// Thruster nozzles: authored exhaust sources for the ship's engine visuals, pinned to
// art pixels exactly like hardpoints (same ship-local art-texel space, +Y = nose).
// `facing` is the direction the EXHAUST leaves the hull (CCW from the nose), so a stern
// main engine faces 180 and a port-flank RCS faces +90. Purely visual: the flight model
// reads none of this. ShipRendering projects the ShipFlight thrust telemetry onto each
// nozzle -- linear thrust against the nozzle's force direction, turn against its torque
// sign -- to decide what burns, so an authored layout responds correctly to strafe,
// reverse and turn without any per-nozzle wiring. A card that authors none falls back
// to the derived stern-plus-corners layout.
// =====================================================================================

#define SHIP_MAX_THRUSTERS 12

enum ThrusterKind : u8 {

    THRUSTER_MAIN = 0,   // the drive: layered flame + idle nozzle glow; burns on forward thrust

    THRUSTER_RCS,        // control jet: short cool puff for strafe / reverse / turn

};

struct ThrusterDef {

    u8            kind;        // ThrusterKind

    bs_math::Vec2 pos_local;   // nozzle exit, ship-local art texels

    f32           facing;      // exhaust direction (radians, CCW from nose = +Y local)

    // Visual multiplier on the jet's width/length. Like HardpointDef::art_scale, a default
    // member initialiser because a zeroed size would draw the jet at nothing.
    f32           size = 1.0f;

};

// Half-extent (ship-local units) of a hardpoint's visual/placement box.

f32 hardpoint_half_extent(HardpointSize s);

// Display letter for a hardpoint/module size ("S"/"M"/"L").

const char* hardpoint_size_name(HardpointSize s);

// Data-driven ship module definition (Phase 4; full declaration in sim/module.h). Ships
// reference immutable registry defs by pointer — modules carry no per-instance state.

struct ModuleDef;

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

// Player sensor suite, matched to the NPC combat profile so both sides of an engagement perceive at
// the same scale (previously the player saw 30k while combat hulls saw millions -- NPCs detected and
// closed long before anything appeared on the player's scope).
//   Layer 1 == the NPC sensor_range (identification / discovery / point-defense reach)
//   Layer 0 == comfort zone; no gameplay effect, kept below Layer 1 to preserve suite ordering
//   Layer 2 == long-range contact; must stay ABOVE layer 1 (the suite is strictly ordered and the
//              editor enforces l0 < l1 < l2), so it sits a step beyond identification range.
#define SENSOR_LAYER0_RADIUS 250000.0f

#define SENSOR_LAYER1_RADIUS 500000.0f

#define SENSOR_LAYER2_RADIUS 1000000.0f

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

// point-defense subsystem resolves to the ship's LIVE Layer 0 sensor radius each frame -- so

// changing Layer 0 changes the laser range with it. Layer 0 is the close-in band: point defense

// is a last-ditch screen and deliberately does NOT reach out to identification range.

// =====================================================================================

// Point-defense doctrine (Phase C, docs/POINT_DEFENSE_AND_MISSILES.md): the player owns
// the WHEN and WHAT of the automated laser, never the aiming.
enum PdStance   : u8 { PD_HOLD = 0, PD_STANDARD = 1, PD_OVERDRIVE = 2 };
enum PdPriority : u8 { PD_PRI_TTI = 0, PD_PRI_GUIDED_FIRST = 1, PD_PRI_NEAREST = 2 };

struct DefenseLaser {

    b8  enabled            = FALSE;   // powered: TRUE only while the fleet's PD device is
                                      // mounted on this hull (set by the mount/unmount paths);
                                      // PD_HOLD is the TACTICAL off, this is the physical one

    u8  stance             = PD_STANDARD;    // HOLD (no fire, no drain) / STANDARD / OVERDRIVE
                                              // (2x dps, 3x drain, 0.5x retarget)

    u8  priority           = PD_PRI_TTI;      // target scoring: impact time / missiles first / nearest

    u8  gate_tier          = 2;       // engagement gate: 0 = 60%, 1 = 80%, 2 = 100% of resolved range

    f32 range              = 5000.0f; // a TRUE last-ditch bubble. PD is the inner layer of the
                                      // missile-defense onion (the flak screen kills at volume
                                      // further out), so its reach is a hull-scale constant.
                                      // 0 opts back into the legacy sensors.layer0_radius
                                      // live-coupling (250k -- 50x the guns' reach, which is
                                      // why it is no longer the default).

    f32 damage_per_second  = 12.0f;   // damage applied to a locked projectile's HP

    f32 dwell_time         = 0.15f;   // seconds the beam tracks one target before releasing

    f32 retarget_cooldown  = 0.08f;   // brief gap before acquiring the next target

    f32 cap_drain_per_s    = 6.0f;    // capacitor drain while a beam is active (Phase B)

    f32 reserve_floor      = 0.15f;   // fraction of cap_max below which the laser stops ACQUIRING
                                      // (an existing lock burns to its dwell end; PD dies last but
                                      // never bottoms the bank on its own)

    // ---- runtime state (not tuning) ----------------------------------------------------

    i32 target_index       = -1;      // locked projectile pool slot; -1 = none

    f32 dwell_remaining    = 0.0f;

    f32 cooldown_remaining = 0.0f;

};

// =====================================================================================
// Ship size classes. Motion feel scales with hull size: a drone flits, a cruiser is a
// slow-turning wall of metal. The class is derived automatically from the hull's WORLD
// length (size_local.y * world_scale) when the card loads, or authored explicitly in the
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

// Speed-limit gears: every hull carries an ascending list of speed-governor settings
// (card line `speed_limits`, or derived from motion.max_speed when unauthored). The
// SELECTED gear -- not motion.max_speed -- is the live velocity cap for both the
// autopilot and the pilot seat; read it through ship_speed_cap so no consumer can
// disagree. The player picks the gear from the fleet panel's SPEED LIMIT chips.

#define SHIP_MAX_SPEED_LIMITS 8
#define SHIP_MIN_SPEED_LIMITS 5

// Classify a hull by its WORLD length (size_local.y * world_scale).

ShipSizeClass ship_size_class_from_length(f32 hull_length_world);

// Motion tuning table lookup for a size class.

const ShipMotion& ship_motion_for_class(ShipSizeClass c);

// Display name ("Drone", "Cruiser", ...).

const char* ship_size_class_name(ShipSizeClass c);

// The live speed cap: the ship's selected speed-limit gear (clamped to the card's list),
// falling back to motion.max_speed when the instance carries no card. The single source
// of truth for every velocity clamp -- the simulate integrator, the autopilot orders and
// the separation pass all read this, never motion.max_speed directly.

struct Ship; // declared below
f32 ship_speed_cap(const Ship* ship);

// Immutable hull stat card this instance was built from (full declaration in
// sim/ship_def.h). Ships reference registry defs by pointer, like weapons do -- the
// registry's fixed pool lives in game_state and outlives every ship.

struct ShipDef;

struct Ship {

    const ShipDef* def;           // the registry stat card (nullptr only for a zeroed Ship)

    f32           world_scale;    // exterior scale multiplier

    ShipSizeClass size_class;     // hull size category (drives motion feel)

    ShipMotion    motion;         // per-ship flight tuning, resolved at load from size_class

    i32           speed_limit_sel; // player-chosen gear: index into def->speed_limits.
                                   // Defaults to the highest gear <= motion.max_speed at
                                   // instantiate, so an untouched hull flies as before.

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

    // ---- Weapon groups (player fire control) --------------------------------------------
    // mount_groups[i] is the group-membership bitmask of the weapon on hardpoints[i]: bit g
    // set = member of group g+1 (selected with number keys 1..SHIP_WEAPON_GROUPS). Non-zero
    // only where mounts[i] holds a weapon; a newly mounted weapon defaults to group 1. The
    // point-defense and AI-piloted ships ignore groups entirely (AI fires via
    // ship_select_bearing_weapon).

    u8             mount_groups[SHIP_MAX_HARDPOINTS];

    u8             active_group;      // selected fire group, 0..SHIP_WEAPON_GROUPS-1

    // ---- Weapon micro-selection override (middle-mouse hub) -----------------------------
    // -1 = "All": the fire selection is the active group's members (the default behaviour).
    // >= 0 = a single hardpoint index that narrows the selection to ONE active-group member —
    // only that weapon fires. It is always a member of the active group: the hub is the only
    // producer and only offers members, selecting a group clears it, and ship_groups_sanitize
    // drops it if the fire-group matrix takes that weapon out of the group. Also cleared by
    // picking "All" in the hub and by any loadout mutation that empties the slot.
    // Consumers still read it as an absolute hardpoint index (no membership re-check at the
    // fire site), so the invariant lives at the three write sites above, not in the readers.
    // Honoured by BOTH the manual fire path and the autopilot attack order.

    // Defaulted here as well as in ship_instantiate: a zero-initialised Ship would otherwise read as
    // "override hardpoint 0" and silently fire one gun, which is the worst possible default.
    i8             weapon_override = -1;

    // ---- Mounted ship modules (Phase 4; one per hardpoint slot) -------------------------
    // module_mounts[i] is the registry ModuleDef installed in hardpoints[i] (nullptr =
    // empty). A hardpoint holds at most ONE occupant across mounts[]/point_defense_mount/
    // module_mounts[]. Installing/removing a module must be followed by
    // ship_recompute_stats() so composed stats (sensors) stay in sync.

    const struct ModuleDef* module_mounts[SHIP_MAX_HARDPOINTS];

    // ---- Unmounted module rack (loadout inventory) --------------------------------------
    // Modules the ship owns but has NOT installed; the Arsenal inspector drags them between
    // this rack and module_mounts[].

    const struct ModuleDef* module_stash[SHIP_MAX_MODULES];

    i32                     module_stash_count;

    // ---- Unmounted weapon stash (loadout inventory) ------------------------------------
    // Weapons the ship owns but has NOT mounted on a hardpoint. The flagship-inspector Arsenal
    // editor drags weapons between this stash (the left "available" list) and mounts[] (the
    // hardpoints on the right). mounts[] and weapon_stash[] never hold the same pointer.

    struct Weapon* weapon_stash[SHIP_MAX_WEAPONS];

    i32            weapon_stash_count; // valid entries in weapon_stash[0..count-1]

    // ---- Legacy shared fire offset (ship-local; fallback when a mount index is -1) ----

    bs_math::Vec2  weapon_fire_offset_local;

    // ---- Per-entity bloom/glow parameters ----------------------------------------------

    bs_glow_params glow;

    // ---- Radiation heat-source emission (0..1) ---------------------------------------

    f32           radiation_emission;

    // ---- Hull integrity (card stat) --------------------------------------------------
    // Combat entity health at spawn, from the card's `hull` line (default 100, the value
    // every hull used before the card existed). CombatArena seeds entity HP from it; the
    // default member initialiser keeps a zero-initialised Ship from spawning at 0 HP.

    f32           hull_max_hp = 100.0f;

    // ---- Sensor suite (three concentric detection layers) ----------------------------
    // `sensors` is the EFFECTIVE suite every consumer reads (discovery, reveal radii,
    // point-defense range). It is DERIVED: ship_recompute_stats() re-composes it from the
    // hull baseline `sensors_base` x mounted sensor modules. Tune the baseline, not it.

    SensorSuite   sensors;

    SensorSuite   sensors_base;

    // ---- Capacitor (shared energy budget: cannons, missile launchers, point-defense) --
    // `cap_max` / `cap_regen` are DERIVED by ship_recompute_stats() from the authored
    // `*_base` hull baselines (capacitor modules will multiply them later, like sensors).
    // `cap_current` is the runtime charge: weapons spend via ship_try_spend_cap at their
    // fire sites, the PD laser drains per beam-second, and ship_capacitor_update regens.
    // See docs/POINT_DEFENSE_AND_MISSILES.md (Phase B).

    f32           cap_max_base;

    f32           cap_regen_base;

    f32           cap_max;

    f32           cap_regen;

    f32           cap_current;

    // ---- Point-defense laser (auto-targets incoming hostile projectiles) -------------

    DefenseLaser  point_defense;

    // Which hardpoint the point-defense currently occupies, or -1 when no PD device is
    // installed on this hull. The DEVICE is fleet-pool equipment (game_state::fleet_pd_stock,
    // one per fleet); this struct is only its per-hull tuning/doctrine storage, inert unless
    // mounted. The simulation gates on enabled AND mount >= 0 (sim/point_defense.cpp).

    i32           point_defense_mount = -1;

    // ---- Turret aim state (Phase 5; drives the procedural mount art) --------------------
    // Per-hardpoint aim angle in SHIP-LOCAL radians (CCW from the nose), slewed toward
    // mount_aim_goal by ship_update_turrets. Gameplay re-asserts the goal every frame via
    // ship_turret_aim_at (cursor / attack target / locked projectile); when nobody does,
    // the turret slews back to the hardpoint's rest facing.

    f32           mount_aim[SHIP_MAX_HARDPOINTS];

    f32           mount_aim_goal[SHIP_MAX_HARDPOINTS];

    b8            mount_aim_engaged[SHIP_MAX_HARDPOINTS]; // goal asserted this frame

    // Per-tick DEFENSE claims by the autonomous flak screen (sim/flak_screen.cpp): set when
    // the screen engages incoming ordnance with a mount, cleared at the top of every
    // flak_screen_update. update_attack yields claimed mounts (no engaging-mount track, no
    // broadside) -- defense preempts offense for the tick, which is what lets an AP cannon
    // serve the screen without the hull-duty aim assertions dragging its barrel off the
    // threat (the ping-pong that starved the original screen).
    b8            flak_screen_claimed[SHIP_MAX_HARDPOINTS];

};

// ---- Hardpoint / mount queries ---------------------------------------------------------

// TRUE when the hardpoint's accepts-mask includes the given MODULE_TYPE_* kind.
b8 hardpoint_accepts(const HardpointDef* hp, u32 module_type);

// First hardpoint accepting `module_type` with no occupant (no weapon, point-defense, or
// module mounted), or -1 when none is free. Used for init-time auto-mounting.
i32 ship_first_free_hardpoint(const Ship* ship, u32 module_type);

// TRUE when the module's kind is accepted by the hardpoint AND its physical size fits
// (a module mounts on slots of its own size or larger).
b8 hardpoint_fits_module(const HardpointDef* hp, const struct ModuleDef* def);

// Re-derive composed ship stats from the hull baseline + mounted modules: effective
// `sensors` = sensors_base with each mounted sensor module's layer multipliers applied.
// Call after any module mount/unmount (ship_instantiate calls it itself).
void ship_recompute_stats(Ship* ship);

// Spend `cost` from the ship's capacitor if affordable. FALSE = starved (hold fire).
// Zero/negative costs always succeed without touching the bank.
b8 ship_try_spend_cap(Ship* ship, f32 cost);

// Continuous capacitor regeneration + clamp to cap_max. Tick wherever the ship's
// weapon cooldowns tick.
void ship_capacitor_update(Ship* ship, f32 dt);

// ---- Point-defense range resolution -----------------------------------------------------
// THE one place the PD engagement-range maths lives: the sim (point_defense.cpp), the range
// ring overlay and the HUD stat card all call these, so what the player sees is what the
// laser does. Resolved range = the tuned/authored range (0 opts into the legacy Layer-0
// sensor coupling) narrowed by the doctrine gate tier.
inline f32 pd_gate_fraction(u8 gate_tier) {
    const f32 GATE_FRAC[3] = { 0.6f, 0.8f, 1.0f };
    return GATE_FRAC[(gate_tier < 3) ? gate_tier : 2];
}
inline f32 pd_resolved_range(const Ship* ship) {
    const DefenseLaser& L = ship->point_defense;
    f32 base = (L.range > 0.0f) ? L.range : ship->sensors.layer0_radius;
    return base * pd_gate_fraction(L.gate_tier);
}

// ---- Per-hardpoint fire origins + traverse arcs (Phase 3) -------------------------------

// World-space fire origin for the given hardpoint: shots leave from the slot's authored
// art-pixel position on the hull. Invalid indices (e.g. -1) fall back to the legacy shared
// weapon_fire_offset_local.
bs_math::HierPos2 ship_hardpoint_fire_origin(const Ship* ship, i32 hp_index);

// The world-space unit EVERY drawn feature of a hardpoint is measured in: its half-extent,
// scaled by the hull and by the slot's own art_scale.
//
// THE one place that product is formed. The mount art, the procedural turret and dish
// rectangles, the barrel origins in ship_muzzle_origin and the charge-up glow all derive from
// it, and a weapon's `muzzle` / `mount_art_size` offsets are authored in it -- so art and shot
// origins scale together and cannot drift apart. Editor affordances (the hardpoint overlay,
// selection highlight, drag feedback and hit-tests) deliberately do NOT use this: they size to
// the SLOT, which art_scale does not change.
f32 ship_hardpoint_unit(const Ship* ship, i32 hp_index);

// World position of one authored BARREL on a mounted weapon, resolved against the turret's
// live slewed aim (mount_aim), in the same hardpoint-half-extent units the art uses. Returns
// the hardpoint centre when the weapon authors no muzzles or `muzzle_index` is negative, which
// keeps un-authored weapons behaving exactly as they did before barrels existed.
//
// THE one place barrel geometry is computed. ship_hardpoint_fire spawns from it and the
// charge-up VFX pass draws on it, so where a shot leaves and where its gun glows cannot drift
// apart -- that failure would have been silent, and only visible mid-traverse.
bs_math::HierPos2 ship_muzzle_origin(const Ship* ship, i32 hp_index, i32 muzzle_index);

// The ship-local angle this mount's barrel WANTS to sit at to bear on `world_dir`, clamped into
// the hardpoint's traverse arc. THE one place that maths lives: ship_turret_aim_at drives the
// slew toward it and ship_weapon_fire_state measures how far the barrel still has to travel, so
// "where the turret is going" and "is it there yet" cannot disagree.
f32 ship_hardpoint_aim_goal(const Ship* ship, i32 hp_index, bs_math::Vec2 world_dir);

// THE spawn site: put this mount's shot(s) in the world, along `world_dir`, with
// `ship_velocity` added. Every fire-control surface routes through it -- the manual
// trigger, the RTS attack order and both NPC gunners -- which is what makes a weapon's
// barrels a data question rather than a per-call-site one.
//
// A weapon whose def authors muzzles fires from those barrel tips, resolved against the
// turret's LIVE aim so the origin sits where the art draws the barrel; MUZZLE_SEQUENTIAL
// advances one barrel per pull, MUZZLE_SALVO empties them all on one cooldown. A weapon
// with no authored muzzles fires one shot from ship_hardpoint_fire_origin, unchanged.
//
// Firing is all this does: the caller still validates with ship_weapon_fire_state and
// commits the capacitor with ship_try_spend_cap first -- a salvo costs one charge.
void ship_hardpoint_fire(Ship* ship, i32 hp_index, bs_math::Vec2 world_dir,
                         bs_math::Vec2 ship_velocity, struct ProjectileSystem* projectiles);

// TRUE when the world-space direction lies inside the hardpoint's traverse arc (centered
// on its facing, rotating with the ship). Full-circle arcs and invalid indices always pass.
b8 ship_hardpoint_can_aim(const Ship* ship, i32 hp_index, bs_math::Vec2 world_dir);

// Hardpoint index of the best mounted weapon able to bear on world_dir: the active weapon
// when it is ready and its arc covers the direction, else the first other ready mounted
// weapon that bears. -1 when no weapon can engage.
i32 ship_select_bearing_weapon(const Ship* ship, bs_math::Vec2 world_dir);

// ---- Turret traverse (Phase 5) ----------------------------------------------------------

// Point the hardpoint's turret at a world-space direction: converts to a ship-local goal
// angle clamped inside the slot's traverse arc and flags the mount engaged for this frame.
void ship_turret_aim_at(Ship* ship, i32 hp_index, bs_math::Vec2 world_dir);

// Tick all turrets one sim step: each mount slews toward its engaged goal (or back to the
// hardpoint's rest facing when idle) at a size-dependent traverse rate, then the engaged
// flags are consumed. Call once per sim tick per ship that renders mount art.
void ship_update_turrets(Ship* ship, f32 dt);

// Ships are built from registry stat cards: see ship_registry_load / ship_instantiate in
// sim/ship_def.h (the old direct-from-file ship_load is retired).

// ---- Weapon groups (player fire control) ----------------------------------------------

// Select fire group g (0-based; key N selects N-1). Also re-homes active_weapon_idx onto
// the group's first member so legacy single-weapon consumers (HUD bar, gizmos) stay sane.
void ship_select_weapon_group(Ship* ship, i32 g);

// TRUE when hardpoint `hp_index` holds a weapon assigned to fire group g. SINGLE SOURCE OF
// TRUTH for the group-membership bit -- every consumer asks this instead of shifting
// mount_groups[] itself, so "what is in this group" cannot be answered two different ways.
b8 ship_hardpoint_in_group(const Ship* ship, i32 hp_index, i32 g);

// Number of mounted weapons whose mask has group g's bit set.
i32 ship_group_size(const Ship* ship, i32 g);

// Hardpoint index of the `n`-th member of group g in hardpoint order (n = 0,1,2,...), or -1
// when the group holds fewer than n+1 weapons. The hub maps its directional slots through
// this, which is what keeps the hub offering the ACTIVE GROUP rather than the whole hull.
i32 ship_nth_group_weapon(const Ship* ship, i32 g, i32 n);

// Enforce the mask invariant after any loadout mutation: empty slots carry no mask, and a
// mounted weapon with no assignment defaults to group 1. Cheap; safe to call every frame.
// Also drops a weapon_override that has stopped being a valid single selection — either its
// slot no longer holds a weapon, or that weapon is no longer in the ACTIVE group. Call it
// after editing group membership, not just after mounting/unmounting.
void ship_groups_sanitize(Ship* ship);

// ---- Weapon micro-selection (middle-mouse hub) -----------------------------------------

// Override the group and fire hardpoint `hp_index` alone. Out-of-range indices and empty
// slots are rejected (the override is left untouched). Also re-homes active_weapon_idx.
void ship_select_weapon_override(Ship* ship, i32 hp_index);

// Restore "All": the fire selection reverts to the active group's members.
void ship_clear_weapon_override(Ship* ship);

// TRUE when hardpoint `hp_index` holds a weapon that the CURRENT selection fires:
// under an override, only the overridden slot; otherwise every active-group member.
// SINGLE SOURCE OF TRUTH for "does this weapon fire on the trigger" -- the manual fire loop,
// the autopilot, the reach ring, the hull digits and the hub all ask this one function.
b8 ship_hardpoint_in_selection(const Ship* ship, i32 hp_index);

// ---- Per-weapon fire validation --------------------------------------------------------
// The five states a weapon can be in for a given shot, in the order the hub renders them.
// WEAPON_FIRE_READY is the only value that permits a shot.
enum WeaponFireState : u8 {
    WEAPON_FIRE_READY = 0,      // in arc, off cooldown, in reach, powered, operational
    WEAPON_FIRE_RELOADING,      // cooling down
    WEAPON_FIRE_OUT_OF_RANGE,   // target beyond weapon_effective_reach (armed, holds fire)
    WEAPON_FIRE_NO_BEARING,     // target outside this mount's traverse arc
    WEAPON_FIRE_SLEWING,        // in arc, but the barrel is still training onto it
    WEAPON_FIRE_STARVED,        // capacitor cannot afford the shot
    WEAPON_FIRE_DISABLED,       // empty slot, or the mount is knocked out
};

// Validate hardpoint `hp_index` against a target: `world_dir` is the fire-origin-to-target
// vector and `dist` its length. Checks operational status, traverse arc, cooldown, reach
// (via weapon_effective_reach -- the single source of truth) and capacitor affordability,
// WITHOUT spending anything. Pass dist < 0 to skip the range test (aim-only queries).
// Both the fire sites and the hub read this, so what the player sees is what the ship does.
WeaponFireState ship_weapon_fire_state(const Ship* ship, i32 hp_index,
                                       bs_math::Vec2 world_dir, f32 dist);

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

