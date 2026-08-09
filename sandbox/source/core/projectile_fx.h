#pragma once

#include <defines.h>

#include <math/math_utils.h>
#include <math/bs_hierpos.h>
#include <renderer/renderer_types.h> // bs_color

// =====================================================================================
// Projectile FX event ring -- the COSMETIC half of every shot.
//
// Three components per shot: a muzzle flash where it leaves the barrel, the streak that
// travels with it (ProjectileSystem::render -- not here), and a termination effect where
// it stops. This file owns the first and third: a fixed ring of short-lived events that
// the simulation WRITES and the render tier READS.
//
// The split is what makes "purely cosmetic" structural rather than a promise:
//
//   * This header is TIER 0. It depends on nothing in the sandbox, so a sim subsystem can
//     include it without inverting the tier order (sim is Tier 1; the renderer is Tier 4).
//   * Nothing in `sim/` ever reads an event back. `emit` returns void and every field is
//     write-once at spawn, so no simulation branch can depend on FX state.
//   * `ProjectileSystem::fx` is a NULLABLE pointer. Setting it to nullptr disables every
//     effect in the game and must leave damage, collision and physics bit-identical --
//     that is the removability test for the whole feature.
//
// FIXED CAPACITY, NEVER FAILS. `emit` overwrites the oldest slot rather than dropping or
// allocating, so the cost is bounded no matter how fast the guns run. Sizing: the hull
// with authored hardpoints has three MEDIUM weapon slots, and the fastest weapon that
// fits them is autocannon_mk1 at 12 shots/s -- 36 events/s from the player flagship on a
// held trigger, plus the enemy hull and NPC gunners. The longest-lived event is 0.32 s,
// so a few dozen are ever concurrent. 128 leaves headroom for a fleet firing at once.
// =====================================================================================

#define MAX_PROJECTILE_FX 128

// What terminated (or started) the shot. The kind picks the lifetime and the look; see
// render/projectile_fx.cpp for the geometry each one draws.
enum ProjectileFxKind : u8 {
    PFX_MUZZLE    = 0, // launch: gas cone at the barrel, pinned in world space
    PFX_IMPACT    = 1, // the shot landed on a hull
    PFX_BURST     = 2, // flak shell proximity-detonated (an airburst, not an impact)
    PFX_INTERCEPT = 3, // point-defense burned the round down mid-flight
};

// Lifetime expiry is deliberately NOT a kind: a shell timing out at maximum range should
// fizzle, not explode. See ProjectileSystem::update, which retires those slots silently.

struct ProjectileFxEvent {

    b8                active;   // FALSE => free slot
    u8                kind;     // ProjectileFxKind
    u8                family;   // VfxFamily (sim/weapon_def.h) -- which visual language to speak
    bs_math::HierPos2 position; // world-space (hierarchical cell + local offset)
    bs_math::Vec2     dir;      // unit; muzzle = barrel axis, impact = incoming heading
    f32               age;      // seconds since emit
    f32               life;     // seconds until retirement (set from `kind`)
    f32               scale;    // world units; the effect's base size
    f32               power;    // ~0.25..3, drives brightness and reach (damage-derived)
    bs_color          tint;     // the shot's own colour, so friend/foe stays readable

};

struct ProjectileFx {

    ProjectileFxEvent events[MAX_PROJECTILE_FX];
    i32               head;  // next ring slot to claim
    i32               live;  // active events, for the profiler panel / diagnostics

    // zero-init; safe to call more than once
    void init();

    // Record one effect. `dir` need not be normalised (zero-length is tolerated and falls
    // back to +Y). `scale` is in world units -- pass the projectile radius for muzzle and
    // impact, the burst radius for flak. `power` scales brightness/size and is normally
    // derived from the shot's damage via projectile_fx_power(). `family` is a VfxFamily and
    // picks which of the three visual languages the effect speaks.
    void emit(u8 kind, u8 family, bs_math::HierPos2 position, bs_math::Vec2 dir,
              f32 scale, f32 power, bs_color tint);

    // Age every live event and retire the expired ones. Call ONCE per frame, EARLY -- before
    // the fire path emits, so a flash spawned this frame is drawn at age 0 instead of
    // already a frame into its 75 ms life.
    void update(f32 dt);

};

// Map hull damage onto the 0.25..3 brightness/size band the effects read. The catalog runs
// from a 6-damage autocannon shell to a 120-damage torpedo, so a torpedo reads heavier than
// an autocannon burst without any per-weapon authoring -- the stat block already says how
// hard the shot hits.
f32 projectile_fx_power(f32 damage);
