#pragma once

#include <defines.h>

#include "sim/ship.h"    // Ship, HardpointDef, SensorSuite, ShipMotion, ShipSizeClass

// =====================================================================================
// Ship definition registry (the hull stat-card system; see the weapon/module registry twins).
//
// A ShipDef is an immutable, data-driven stat card for one hull type, loaded from a
// `.ship` text file. Every hull in the game -- player, enemy, NPC archetypes -- is one
// card; ship INSTANCES are built from cards by ship_instantiate. The card owns everything
// authored: identity, art, footprint, collider, hardpoint skeleton, and the gameplay
// stats. All lines except `id` are optional and default to the values the game used
// before the line existed:
//
//   id          vanguard_cruiser     # registry key (REQUIRED; file is skipped without it)
//   name        "Vanguard Cruiser"   # display name (defaults to the id)
//   desc        "Line cruiser ..."   # player-facing role description (stat-card paragraph)
//   faction     2                    # VesselFaction default; spawn sites override it
//   world_scale 1.0
//   size        394 1586             # art footprint (ship-local texels, w h)
//   class       cruiser              # authored size class; omitted = derived from length
//   hull        100                  # max hull HP (combat entity health at spawn)
//   motion      240 90 70 1.4 0.45   # max_speed accel decel turn_accel max_turn;
//                                    # omitted = the size class's tuning table
//   capacitor   100 8                # cap_max cap_regen baselines
//   sensors     250000 500000 1e6    # sensor baseline l0 l1 l2 (strictly increasing)
//   price       12000                # market-forward: credits (unused v1)
//   tier        1                    # market-forward: tech tier / rarity band
//
// plus the long-standing art/skeleton lines, unchanged in meaning:
//
//   layer sprite <tex> <ox> <oy> <ppu> <z> <roof>
//   layer mapped <diffuse> <normal> <depth> <position> <ox> <oy> <ppu> <z> <roof>
//   collider <x> <y>                          # repeated; omitted = box from `size`
//   hardpoint <id> <accepts> <size> <x> <y> <facing_deg> <arc_deg> [art_scale]
//   thruster <main|rcs> <x> <y> <facing_deg> [size]   # repeated; exhaust nozzles pinned
//                                    # to art pixels (facing = the direction the exhaust
//                                    # LEAVES the hull, CCW from nose: 180 = stern main,
//                                    # 90 = port flank). Visual only; omitted = the draw
//                                    # pass derives a stern-plus-corners layout.
//
// The registry is a manifest (assets/ships/ships.list) naming one `.ship` path per
// line. Defs load once at game init into a fixed pool; textures resolve in the
// mandatory post-renderer second phase (ship_registry_resolve_textures), so instances
// created AFTER that share the def's resolved handles (instances created before it
// keep their own ship_visual_resolve_textures pass, as game_init's fleet does).
// A Ship instance points its `def` and `vessel_name` INTO the registry's pool storage
// (safe: the fixed pool never reallocates and outlives every ship in game_state).
// =====================================================================================

#define SHIP_REGISTRY_MAX 32

struct ShipDef {

    char          id[32];         // registry key ("vanguard_cruiser"), from the .ship file
    char          name[48];       // display name; defaults to the id
    char          desc[192];      // player-facing role description (stat-card paragraph)
    VesselFaction faction;        // authored default; every spawn site overrides it

    f32           world_scale;    // exterior scale multiplier
    bs_math::Vec2 size_local;     // art footprint (ship-local texels, unscaled)
    ShipSizeClass size_class;     // resolved: authored `class`, else derived from world length
    ShipMotion    motion;         // resolved: authored `motion`, else the class tuning table
    b8            motion_authored;// TRUE when the card overrode the class table

    f32           hull_max_hp;    // combat entity health at spawn (default 100)
    b8            hull_authored;  // TRUE when the card authored `hull` -- consumers with
                                  // their own tuning (NPC archetypes) fall back when FALSE
    f32           cap_max_base;   // capacitor baselines (modules multiply on the instance)
    f32           cap_regen_base;
    SensorSuite   sensors_base;   // sensor baseline (modules multiply on the instance)

    bs_math::Vec2 collider_verts[SHIP_MAX_COLLIDER_VERTS]; // convex, CCW, ship-local
    i32           collider_count;

    HardpointDef  hardpoints[SHIP_MAX_HARDPOINTS];
    i32           hardpoint_count;

    ThrusterDef   thrusters[SHIP_MAX_THRUSTERS];   // authored exhaust nozzles (visual only;
    i32           thruster_count;                  // 0 = the draw pass derives a layout)

    ShipVisual    visual;         // layer paths at load; handles live after resolve;
                                  // size_local here is pre-multiplied by world_scale

    i32           price;          // market-forward: credits (unused v1)
    i32           tier;           // market-forward: tech tier (unused v1)

};

struct ShipRegistry {

    ShipDef defs[SHIP_REGISTRY_MAX];

    i32     count;

};

// Load every `.ship` file named in the manifest (one path per line, '#' comments).
// Returns FALSE only when the manifest itself cannot be opened; individual bad files
// (unreadable, no id, no visual layers, duplicate id) are skipped with a warning.
// Call once during game init, BEFORE any ship_instantiate.
b8 ship_registry_load(ShipRegistry* reg, const char* manifest_path);

// Resolve every def's visual layer paths into live texture handles. Mandatory second
// phase, exactly like weapon_registry_resolve_textures: the registry loads before the
// renderer exists, so it can only record path strings. Call once at game init AFTER the
// renderer is initialized; instances created after this copy the resolved handles.
void ship_registry_resolve_textures(ShipRegistry* reg);

// Find a loaded def by its registry id, or nullptr.
const ShipDef* ship_registry_find(const ShipRegistry* reg, const char* id);

// Build a ship instance from a def: placement-news *out_ship (game_state slots are raw
// memory), copies the card's data, points vessel_name/def into the registry pool, and
// runs the runtime init the old ship_load did -- mounts cleared, turrets parked at rest,
// stats recomputed, capacitor full, origin {0,0}. Returns FALSE on a null def/ship.
b8 ship_instantiate(const ShipDef* def, Ship* out_ship);
