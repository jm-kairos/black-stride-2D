#pragma once

#include <defines.h>

#include <renderer/renderer_types.h>  // bs_texture (resolved mount art)

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
// Optional in-world turret art (ShipRendering draws it in place of the procedural
// rectangles; a def with no `mount_art` keeps the rectangles, which is why railguns and
// missile racks are untouched by cannons getting art):
//
//   mount_art       assets/weapons/assets/tier_1_cannon_single_barrel.png
//   mount_art_size  1.85 3.00        # width, height of the quad
//   mount_art_pivot -0.32            # rotation centre, offset from the art's own centre
//
// All three geometry numbers are in units of the HARDPOINT half-extent
// (hardpoint_half_extent, x world_scale), so the art scales with the slot it sits in
// exactly as the procedural rectangles did. Height runs along the barrel axis; the art
// must be authored barrel-UP, matching the hull convention (angle 0 = nose = +Y).
// `mount_art_pivot` is measured along that same axis, positive toward the muzzle — it
// exists because a turret's rotation centre is its base, not the middle of a quad that
// also contains the barrel.
//
// Optional barrels. Each `muzzle` line adds one, as (right, forward) FROM THE HARDPOINT,
// in those same half-extent units and in the weapon's own frame -- so they rotate with the
// turret's live aim and stay glued to the barrels the art draws:
//
//   muzzle -0.51 1.82                # port barrel
//   muzzle  0.00 1.82                # centre barrel
//   muzzle  0.51 1.82                # starboard barrel
//   muzzle_pattern sequential        # sequential (default) | salvo
//
// A weapon with no `muzzle` line fires from the hardpoint centre exactly as before, which
// is what leaves every un-authored weapon untouched. Giving a weapon barrels is therefore a
// data edit: `ship_hardpoint_fire` is the only spawn site, so the manual trigger, the
// autopilot and NPC gunners all pick them up together.
//
// The registry is a manifest (assets/weapons/weapons.list) naming one `.weapon` path
// per line. Defs load once at game init into a fixed pool; weapon INSTANCES are built
// from defs by weapon_instantiate and point their name/icon INTO the def's storage
// (safe: the pool is fixed and never reallocates).
// =====================================================================================

#define WEAPON_REGISTRY_MAX 32

// How many barrels one weapon may author. Raise freely -- the array is per-def, and the
// registry holds 32 defs.
#define WEAPON_MAX_MUZZLES 8

// What a single trigger pull does when a weapon has more than one barrel.
//   SEQUENTIAL -- one shot, from the next barrel in rotation. Rate, damage and capacitor
//                 cost are untouched, so a gun's authored economy survives being re-modelled
//                 with barrels; at any real fire rate the barrels visibly take turns.
//   SALVO      -- one shot from EVERY barrel at once, for one capacitor charge. This
//                 multiplies the weapon's damage per pull by its barrel count, so a def that
//                 opts in must be balanced for it.
enum MuzzlePattern : u8 { MUZZLE_SEQUENTIAL = 0, MUZZLE_SALVO = 1 };

struct WeaponDef {

    char          id[32];        // registry key ("gauss_mk1")
    char          name[48];      // display name
    char          icon[16];      // ui-icons emblem sprite name
    char          desc[192];     // player-facing role description (stat-card paragraph)
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

    // ---- In-world mount art (optional; empty path = procedural rectangles) ----
    char          mount_art[128];   // turret art path, resolved to a texture after the renderer is up
    f32           mount_art_w;      // quad width,  in units of the hardpoint half-extent
    f32           mount_art_h;      // quad height, in units of the hardpoint half-extent (barrel axis)
    f32           mount_art_pivot;  // rotation centre off the quad's centre, same units, + = muzzle
    bs_texture    mount_art_tex;    // resolved handle (0 until weapon_registry_resolve_textures)

    // ---- Barrels (optional; muzzle_count 0 = fire from the hardpoint centre) ----
    bs_math::Vec2 muzzles[WEAPON_MAX_MUZZLES]; // (right, forward) from the hardpoint, half-extents
    i32           muzzle_count;                // authored barrels; 0 keeps the legacy single origin
    u8            muzzle_pattern;              // MuzzlePattern: what one trigger pull does

};

struct WeaponRegistry {

    WeaponDef defs[WEAPON_REGISTRY_MAX];

    i32       count;

};

// Load every `.weapon` file named in the manifest (one path per line, '#' comments).
// Returns FALSE only when the manifest itself cannot be opened; individual bad files
// are skipped with a warning. Call once during game init.
b8 weapon_registry_load(WeaponRegistry* reg, const char* manifest_path);

// Resolve every def's `mount_art` path into a live texture handle. Mandatory second phase,
// exactly like ship_visual_resolve_textures: weapon_registry_load runs before the renderer
// exists, so it can only record path strings. Call once at game init AFTER the renderer is
// initialized; defs with no mount_art are skipped and keep drawing procedurally.
void weapon_registry_resolve_textures(WeaponRegistry* reg);

// Find a loaded def by its registry id, or nullptr.
const WeaponDef* weapon_registry_find(const WeaponRegistry* reg, const char* id);

// Build a heap weapon instance (BallisticWeapon or MissileLauncher) from a def. The
// instance's name/icon point into the def's pool storage. Returns nullptr on bad def.
struct Weapon* weapon_instantiate(const WeaponDef* def, VesselFaction owner);
