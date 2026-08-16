#include "core/projectile_fx.h"

#include <math.h>

using namespace bs_math;

// Per-kind lifetimes, in seconds. Grounded-kinetic: everything is SHORT. A muzzle flash is
// the gas leaving the barrel, not a lingering fireball, and at 12 shots/s a flash that
// outlived its own cooldown (83 ms) would smear into a continuous glow and stop reading as
// discrete shots. Impact gets the longest life because it is the beat the player is
// watching for.
static const f32 FX_LIFE[4] = {
    0.075f, // PFX_MUZZLE
    0.280f, // PFX_IMPACT
    0.320f, // PFX_BURST
    0.140f, // PFX_INTERCEPT
};

void ProjectileFx::init() {
    for (i32 i = 0; i < MAX_PROJECTILE_FX; ++i)
        events[i].active = FALSE;
    head = 0;
    live = 0;
}

// Per-family lifetime scaling, applied on top of FX_LIFE. Section 17 of the VFX reference asks
// for different temporal scales rather than one duration for everything, and the three families
// genuinely differ: a rail launch is an electromagnetic snap with nothing left to burn, while a
// missile ignition and a warhead detonation both have fuel and both linger.
static const f32 FAMILY_LIFE_MUL[3] = {
    1.00f, // VFX_SHELL
    0.80f, // VFX_SLUG
    1.55f, // VFX_ORDNANCE
};

void ProjectileFx::emit(u8 kind, u8 family, HierPos2 position, Vec2 dir,
                        f32 scale, f32 power, bs_color tint)
{
    // Claim the next ring slot unconditionally. Overwriting the oldest event is the correct
    // failure mode for a cosmetic buffer: it cannot fail, cannot allocate, and cannot stall
    // the fire path, and the event it discards is the one already closest to expiry.
    ProjectileFxEvent& e = events[head];
    head = (head + 1) % MAX_PROJECTILE_FX;

    if (!e.active) ++live;   // reclaiming a live slot leaves the count unchanged

    // Normalise here so no draw site has to. A zero-length direction is possible in
    // principle (a projectile retired the frame it spawned) and must not produce a NaN
    // rotation, so it falls back to the ship convention's "up".
    f32 len = vec2_length(dir);
    Vec2 unit = (len > 1.0e-4f) ? vec2_scale(dir, 1.0f / len) : Vec2{ 0.0f, 1.0f };

    e.active   = TRUE;
    e.kind     = (kind < 4) ? kind : (u8)PFX_IMPACT;
    e.family   = (family < 3) ? family : (u8)0;   // unknown family degrades to SHELL
    e.position = position;
    e.dir      = unit;
    e.age      = 0.0f;
    e.life     = FX_LIFE[e.kind] * FAMILY_LIFE_MUL[e.family];
    e.scale    = (scale > 0.0f) ? scale : 1.0f;
    e.power    = power;
    e.tint     = tint;
}

void ProjectileFx::update(f32 dt) {
    i32 active_count = 0;
    for (i32 i = 0; i < MAX_PROJECTILE_FX; ++i) {
        ProjectileFxEvent& e = events[i];
        if (!e.active) continue;
        e.age += dt;
        if (e.age >= e.life) e.active = FALSE;
        else                 ++active_count;
    }
    live = active_count;
}

f32 projectile_fx_power(f32 damage) {
    // gauss_mk1's 15 damage is the catalog's reference point (its damage-per-capacitor is
    // what everything else is tuned against), so it anchors power at 1.0. The clamp keeps a
    // 6-damage autocannon shell visible and stops a 120-damage torpedo from filling the
    // screen: the band is 0.25..3, not the raw 0.4..8 the division would give.
    f32 p = damage / 15.0f;
    if (p < 0.25f) p = 0.25f;
    if (p > 3.0f)  p = 3.0f;
    return p;
}
