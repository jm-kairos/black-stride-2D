#pragma once
#include <defines.h>
#include <math/math_utils.h>
#include <math/bs_hierpos.h>
#include <renderer/renderer_types.h>
#include "sim/ship.h"
#define MAX_PROJECTILES 512
// Projectile kinds: plain shells fly straight; missiles are steered toward hostile combat
// entities each tick by combat_arena_steer_missiles (flight model in game_state::missile_tuning);
// flak shells proximity-detonate against hostile ordnance (game_state::flak_tuning) and never
// damage ships.
enum ProjectileKind : u8 { PROJ_SHELL = 0, PROJ_MISSILE = 1, PROJ_FLAK = 2 };
struct Projectile {
    b8            active;     // FALSE => free slot
    u8            kind;       // PROJ_SHELL / PROJ_MISSILE
    bs_math::HierPos2 position;   // world-space (hierarchical grid cell + local offset)
    bs_math::Vec2 velocity;   // world-space units/s
    f32           lifetime;   // seconds remaining
    f32           age;        // seconds since spawn
    f32           radius;     // visual radius (world units)
    f32           radiation_emission; // 0..1 heat-source strength; 0 means invisible to detector
    bs_color      color;      // tint
    VesselFaction owner;      // who fired it (legacy binary faction; kept for visuals/back-compat)
    i16           faction_id; // unified attacker faction (civ index / FACTION_PLAYER / FACTION_PIRATE)
    f32           hp;         // health; point-defense lasers deplete this, <=0 => destroyed
    f32           max_hp;     // HP at spawn (for beam intensity / future UI)
    f32           dmg;        // hull damage applied on impact (stamped from the firing weapon)
    f32           max_speed;  // PROJ_MISSILE only: this seeker's own velocity clamp, stamped from
                              // the firing launcher's proj_speed. The guidance pass clamps to THIS
                              // rather than a global, so a .weapon file's proj_speed actually
                              // governs the missile's flight (and therefore its reach). 0 => fall
                              // back to the global missile_tuning clamp (non-missile projectiles).
};
struct ProjectileSystem {
    Projectile pool[MAX_PROJECTILES];
    i32        count;   // number of active projectiles
    bs_texture streak_texture; // 2D tapered bullet streak
    bs_texture flash_texture;  // radial gradient for head orb / muzzle flash
    // zero-init
    void init();
    // spawn a new projectile; returns TRUE if a free slot was found. `faction_id` is the unified
    // attacker faction (civ index / FACTION_PLAYER / FACTION_PIRATE) used for hit filtering and
    // kill attribution; `owner` remains the legacy binary faction for visuals/back-compat.
    b8 spawn(bs_math::HierPos2 origin, bs_math::Vec2 velocity,
             f32 lifetime, f32 radius, bs_color color, VesselFaction owner, i16 faction_id,
             f32 radiation_emission = 0.0f, f32 hp = 1.0f, u8 kind = PROJ_SHELL, f32 dmg = 15.0f);
    // spawn a guided missile (kind = PROJ_MISSILE): same pool, fat HP so point-defense needs a
    // dwell to kill it. Steering happens in the combat-arena pass, not here. `max_speed` is the
    // seeker's own velocity clamp (the launcher's proj_speed); 0 => use the global tuning clamp.
    b8 spawn_missile(bs_math::HierPos2 origin, bs_math::Vec2 velocity,
                     f32 lifetime, f32 radius, bs_color color, VesselFaction owner, i16 faction_id,
                     f32 radiation_emission, f32 hp, f32 dmg = 40.0f, f32 max_speed = 0.0f);
    // advance all active projectiles; retire those whose lifetime expired
    void update(f32 dt);
    // submit draw commands for all active projectiles. Positions are transformed into render
    // space relative to `camera` (render = hierpos_diff(pos, camera)).
    void render(u32 layer, const bs_math::HierPos2* camera) const;
    const bs_glow_params* glow_override; // NULL => use global glow; else per-bullet glow
};
