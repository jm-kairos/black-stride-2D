#pragma once
#include <defines.h>
#include <math/math_utils.h>
#include <renderer/renderer_types.h>
#include "ship.h"
#define MAX_PROJECTILES 256
struct Projectile {
    b8            active;     // FALSE => free slot
    bs_math::Vec2 position;   // world-space
    bs_math::Vec2 velocity;   // world-space units/s
    f32           lifetime;   // seconds remaining
    f32           age;        // seconds since spawn
    f32           radius;     // visual radius (world units)
    bs_color      color;      // tint
    VesselFaction owner;      // who fired it
};
struct ProjectileSystem {
    Projectile pool[MAX_PROJECTILES];
    i32        count;   // number of active projectiles
    bs_texture streak_texture; // 2D tapered bullet streak
    bs_texture flash_texture;  // radial gradient for head orb / muzzle flash
    // zero-init
    void init();
    // spawn a new projectile; returns TRUE if a free slot was found
    b8 spawn(bs_math::Vec2 origin, bs_math::Vec2 velocity,
             f32 lifetime, f32 radius, bs_color color, VesselFaction owner);
    // advance all active projectiles; retire those whose lifetime expired
    void update(f32 dt);
    // submit draw commands for all active projectiles
    void render(u32 layer) const;
    const bs_glow_params* glow_override; // NULL => use global glow; else per-bullet glow
};
