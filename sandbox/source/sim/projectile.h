#pragma once
#include <defines.h>
#include <math/math_utils.h>
#include <math/bs_hierpos.h>
#include <renderer/renderer_types.h>
#include "sim/ship.h"
#include "core/projectile_fx.h"  // ProjectileFx (cosmetic muzzle/impact ring; Tier 0)
#define MAX_PROJECTILES 512

// How many past positions a guided round remembers for its curved trail, and how often one is
// taken. COSMETIC ONLY -- see Projectile::trail.
//
// Sampled on a fixed TIME interval rather than per frame, so the trail is the same length at 30
// and 144 fps; a per-frame history would make the trail a readout of the player's hardware.
// 8 samples x 25 ms is 200 ms of history, which at harpoon speed is ~2000 world units -- a bit
// longer than the hull that fired it, enough to read an arc without smearing the screen.
#define PROJ_TRAIL_SAMPLES  8
#define PROJ_TRAIL_INTERVAL 0.025f
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
    // ---- Curved-trail history (COSMETIC ONLY; guided rounds only) -----------------------
    // Past positions as OFFSETS FROM `position`, newest at [0]. Offsets rather than absolute
    // HierPos2 for two reasons: 8 bytes a sample instead of 24, and a short local trail is
    // trivially precision-safe as a relative Vec2 no matter how far from the origin the fight
    // is. The cost is that every sample must be rebased by each tick's movement -- but a sample
    // only lives ~200 ms, so it is rebased about a dozen times before being overwritten and
    // accumulated float drift never gets the chance to matter.
    //
    // Recorded ONLY for PROJ_MISSILE, so the per-tick cost is bounded by missiles in flight
    // rather than by the 512-slot pool. Read only by ProjectileSystem::render.
    bs_math::Vec2 trail[PROJ_TRAIL_SAMPLES];
    u8            trail_count;  // valid samples in `trail`
    f32           trail_timer;  // seconds until the next sample is taken
    u8            vfx_family; // VfxFamily, stamped from the firing weapon's def. COSMETIC ONLY:
                              // read by ProjectileSystem::render and the FX pass, by nothing in
                              // the simulation. Two projectiles differing only in this field
                              // fly, collide and damage identically.
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
             f32 radiation_emission = 0.0f, f32 hp = 1.0f, u8 kind = PROJ_SHELL, f32 dmg = 15.0f,
             u8 vfx_family = 0 /* VFX_SHELL */);
    // spawn a guided missile (kind = PROJ_MISSILE): same pool, fat HP so point-defense needs a
    // dwell to kill it. Steering happens in the combat-arena pass, not here. `max_speed` is the
    // seeker's own velocity clamp (the launcher's proj_speed); 0 => use the global tuning clamp.
    b8 spawn_missile(bs_math::HierPos2 origin, bs_math::Vec2 velocity,
                     f32 lifetime, f32 radius, bs_color color, VesselFaction owner, i16 faction_id,
                     f32 radiation_emission, f32 hp, f32 dmg = 40.0f, f32 max_speed = 0.0f,
                     u8 vfx_family = 2 /* VFX_ORDNANCE */);
    // advance all active projectiles; retire those whose lifetime expired
    void update(f32 dt);
    // Free slot `index` and record the matching termination effect. THE one place a shot
    // stops for a reason -- lifetime expiry is handled inside update() instead, silently,
    // because a shell timing out at maximum range should fizzle rather than explode.
    // `fx_kind` is a ProjectileFxKind: the caller says WHY, because only the caller knows
    // whether the round hit a hull, was burned down, or detonated on its own fuse.
    // `fx_scale` overrides the effect's base size (0 => the projectile's own radius); flak
    // passes its burst radius so the airburst is drawn at the size that actually did damage.
    // Frees the slot whether or not FX are attached, so it is a drop-in for the
    // `active = FALSE; --count` pairs it replaces.
    void retire(i32 index, u8 fx_kind, f32 fx_scale = 0.0f);
    // submit draw commands for all active projectiles. Positions are transformed into render
    // space relative to `camera` (render = hierpos_diff(pos, camera)).
    void render(u32 layer, const bs_math::HierPos2* camera) const;
    const bs_glow_params* glow_override; // NULL => use global glow; else per-bullet glow
    // Cosmetic muzzle/impact ring, wired once at init to the game_state member. NULL disables
    // every launch and termination effect in the game and must leave damage, collision and
    // physics bit-identical -- that nullability IS the removability test for the VFX feature.
    // spawn/spawn_missile emit the muzzle flash from here, which is what makes the effect
    // reach EVERY fire path: the manual trigger, the autopilot, both combat-arena gunners and
    // the NPC agent gunner that bypasses ship_hardpoint_fire all funnel through these two.
    ProjectileFx* fx;
};
