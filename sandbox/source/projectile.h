#pragma once
#include <defines.h>
#include <math/math_utils.h>
#include <math/bs_hierpos.h>
#include <renderer/renderer_types.h>
#include "ship.h"
#define MAX_PROJECTILES 256
struct Projectile {
    b8            active;     // FALSE => free slot
    bs_math::HierPos2 position;   // world-space (hierarchical grid cell + local offset)
    bs_math::Vec2 velocity;   // world-space units/s
    f32           lifetime;   // seconds remaining
    f32           age;        // seconds since spawn
    f32           radius;     // visual radius (world units)
    f32           radiation_emission; // 0..1 heat-source strength; 0 means invisible to detector
    bs_color      color;      // tint
    VesselFaction owner;      // who fired it
    f32           hp;         // health; point-defense lasers deplete this, <=0 => destroyed
    f32           max_hp;     // HP at spawn (for beam intensity / future UI)
};
struct ProjectileSystem {
    Projectile pool[MAX_PROJECTILES];
    i32        count;   // number of active projectiles
    bs_texture streak_texture; // 2D tapered bullet streak
    bs_texture flash_texture;  // radial gradient for head orb / muzzle flash
    // zero-init
    void init();
    // spawn a new projectile; returns TRUE if a free slot was found
    b8 spawn(bs_math::HierPos2 origin, bs_math::Vec2 velocity,
             f32 lifetime, f32 radius, bs_color color, VesselFaction owner,
             f32 radiation_emission = 0.0f, f32 hp = 1.0f);
    // advance all active projectiles; retire those whose lifetime expired
    void update(f32 dt);
    // submit draw commands for all active projectiles. Positions are transformed into render
    // space relative to `camera` (render = hierpos_diff(pos, camera)).
    void render(u32 layer, const bs_math::HierPos2* camera) const;
    const bs_glow_params* glow_override; // NULL => use global glow; else per-bullet glow
};
