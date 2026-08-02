#pragma once

#include <defines.h>

#include "sim/ship.h"    // HardpointSize
#include "sim/weapon.h"  // WeaponKind, Weapon

// =====================================================================================
// Weapon definition registry (stat system S2; see docs and the module registry twin).
//
// A WeaponDef is an immutable, data-driven stat block for a mountable weapon, loaded
// from a `.weapon` text file:
//
//   id          gauss_mk1
//   name        "Gauss Cannon Mk I"
//   kind        ballistic            # ballistic | missile
//   size        M                    # S/M/L; mounts on hardpoints of this size or larger
//   icon        ic-cannon            # ui-icons emblem sprite
//   damage      15                   # hull damage per hit (stamped into the projectile)
//   fire_rate   5.0                  # ballistic: shots/s
//   reload      4.0                  # missile: seconds per tube cycle
//   proj_speed  12000                # launch speed; reach = speed * proj_life
//   proj_life   20
//   proj_radius 4.0
//   cap_cost    4                    # capacitor per trigger pull (the economy lever)
//   emission    0.6                  # projectile sensor visibility (stealth lever)
//   proj_hp     1.0                  # how hard point-defense/flak kill the projectile
//   price       400                  # market-forward: credits (unused until markets trade weapons)
//   tier        1                    # market-forward: tech tier / rarity band
//
// The registry is a manifest (assets/weapons/weapons.list) naming one `.weapon` path
// per line. Defs load once at game init into a fixed pool; weapon INSTANCES are built
// from defs by weapon_instantiate and point their name/icon INTO the def's storage
// (safe: the pool is fixed and never reallocates).
// =====================================================================================

#define WEAPON_REGISTRY_MAX 32

struct WeaponDef {

    char          id[32];        // registry key ("gauss_mk1")
    char          name[48];      // display name
    char          icon[16];      // ui-icons emblem sprite name
    u8            kind;          // WEAPON_KIND_BALLISTIC / WEAPON_KIND_MISSILE
    HardpointSize size;          // physical size; mounts on slots of this size or larger
    f32           damage;        // hull damage per hit
    f32           fire_rate;     // ballistic: shots per second
    f32           reload;        // missile: seconds per tube cycle
    f32           proj_speed;    // projectile/launch speed (world units/s)
    f32           proj_life;     // projectile lifetime (s); reach = speed * life
    f32           proj_radius;   // projectile visual/collision radius
    f32           cap_cost;      // capacitor per trigger pull
    f32           emission;      // projectile radiation emission (0..1)
    f32           proj_hp;       // projectile HP vs point-defense/flak
    i32           price;         // market-forward: credits (unused v1)
    i32           tier;          // market-forward: tech tier (unused v1)

};

struct WeaponRegistry {

    WeaponDef defs[WEAPON_REGISTRY_MAX];

    i32       count;

};

// Load every `.weapon` file named in the manifest (one path per line, '#' comments).
// Returns FALSE only when the manifest itself cannot be opened; individual bad files
// are skipped with a warning. Call once during game init.
b8 weapon_registry_load(WeaponRegistry* reg, const char* manifest_path);

// Find a loaded def by its registry id, or nullptr.
const WeaponDef* weapon_registry_find(const WeaponRegistry* reg, const char* id);

// Build a heap weapon instance (BallisticWeapon or MissileLauncher) from a def. The
// instance's name/icon point into the def's pool storage. Returns nullptr on bad def.
struct Weapon* weapon_instantiate(const WeaponDef* def, VesselFaction owner);
