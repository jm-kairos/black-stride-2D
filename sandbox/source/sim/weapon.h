#pragma once
#include <defines.h>
#include <math/math_utils.h>
#include "sim/ship.h"
#include "sim/projectile.h"
// ---------------------------------------------------------------------------
// Weapon base class — firing interface, cooldown handling, generic behaviour.
// ---------------------------------------------------------------------------
struct Weapon {
    const char*   name;           // display label
    VesselFaction owner_faction;  // set at equip time (legacy binary faction; projectile visuals)
    i16           owner_faction_id; // unified faction stamped from the firing ship at each fire site
    Weapon(const char* n) : name(n), owner_faction((VesselFaction)0), owner_faction_id(FACTION_PIRATE) {}
    virtual ~Weapon() {}
    // fire at the given origin in the given direction (unit or non-unit).
    // `ship_velocity` is the owning ship's current world velocity (added to projectile).
    // `projectiles` is the global projectile manager to spawn into.
    virtual void fire(bs_math::HierPos2 origin, bs_math::Vec2 direction,
                      bs_math::Vec2 ship_velocity,
                      ProjectileSystem* projectiles) = 0;
    // per-frame update (cooldowns, charge states, etc.)
    virtual void update(f32 dt) = 0;
    // TRUE if the weapon can fire right now (cooldown expired)
    virtual b8 ready() const = 0;
    // 0..1 charge / cooldown progress (for UI)
    virtual f32 cooldown_progress() const = 0;
    // projectile speed for aim prediction (0 if not applicable)
    virtual f32 projectile_speed() const { return 0.0f; }
};
// ---------------------------------------------------------------------------
// Ballistic weapon — fires physical projectiles.
// ---------------------------------------------------------------------------
struct BallisticWeapon : Weapon {
    // tuning
    f32 fire_rate;          // shots per second
    f32 projectile_speed_value;  // world units per second
    f32 projectile_lifetime;// seconds before auto-cleanup
    f32 projectile_radius;  // visual radius (world units)
    f32 projectile_emission; // 0..1 radiation heat-source strength for the projectile
    // runtime state
    f32 cooldown_remaining; // seconds until next shot
    f32 cooldown_duration;  // 1.0f / fire_rate (cached)
    BallisticWeapon(const char* name,
                    f32 rate,
                    f32 speed,
                    f32 lifetime,
                    f32 radius,
                    f32 emission = 0.5f);
    void fire(bs_math::HierPos2 origin, bs_math::Vec2 direction,
              bs_math::Vec2 ship_velocity,
              ProjectileSystem* projectiles) override;
    void update(f32 dt) override;
    b8   ready() const override;
    f32  cooldown_progress() const override;
    f32  projectile_speed() const override { return projectile_speed_value; }
};
// ---------------------------------------------------------------------------
// Helper: factory for creating the default ballistic cannon.
// ---------------------------------------------------------------------------
Weapon* weapon_create_ballistic_cannon(VesselFaction owner);
