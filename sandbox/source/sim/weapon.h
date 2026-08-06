#pragma once
#include <defines.h>
#include <math/math_utils.h>
#include "sim/ship.h"
#include "sim/projectile.h"
// ---------------------------------------------------------------------------
// Weapon base class — firing interface, cooldown handling, generic behaviour.
// ---------------------------------------------------------------------------
// Weapon kind tag (no RTTI in the hot path): set by each subclass constructor.
enum WeaponKind : u8 { WEAPON_KIND_BALLISTIC = 0, WEAPON_KIND_MISSILE = 1 };
// Ballistic fire modes (Phase D): AP shells kill ships; FLAK shells fly slow/short and
// proximity-detonate against hostile ordnance (missiles + shells), never hulls.
enum FireMode : u8 { MODE_AP = 0, MODE_FLAK = 1 };
struct WeaponDef;   // sim/weapon_def.h (full stat block; instances keep a pointer)
struct Weapon {
    const char*   name;           // display label
    const char*   icon;           // ui-icons emblem sprite for the Arsenal tile ("ic-cannon")
    const WeaponDef* def;         // originating stat block (nullptr only for legacy paths)
    u8            wkind;          // WeaponKind tag for kind-specific fire logic
    u8            size;           // HardpointSize: mounts on slots of this size or larger
    f32           damage;         // hull damage per hit (stamped into spawned projectiles)
    VesselFaction owner_faction;  // set at equip time (legacy binary faction; projectile visuals)
    i16           owner_faction_id; // unified faction stamped from the firing ship at each fire site
    Weapon(const char* n) : name(n), icon("ic-cannon"), def(nullptr), wkind(WEAPON_KIND_BALLISTIC), size(HARDPOINT_MEDIUM), damage(15.0f), owner_faction((VesselFaction)0), owner_faction_id(FACTION_PIRATE) {}
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
    // capacitor cost per trigger pull (spent at the fire site via ship_try_spend_cap;
    // 0 = free). See docs/POINT_DEFENSE_AND_MISSILES.md (Phase B).
    virtual f32 cap_cost() const { return 0.0f; }
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
    f32 cap_cost_value;     // capacitor cost per shot (member, editor-exposable)
    f32 proj_hp_value;      // spawned shell HP vs point-defense/flak (def-driven)
    u8  fire_mode;          // MODE_AP / MODE_FLAK (toggled per fire group via the T key)
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
    f32  cap_cost() const override { return cap_cost_value; }
};
// ---------------------------------------------------------------------------
// Missile launcher — fires seeker missiles (PROJ_MISSILE). Fire-and-seek: the
// launcher needs no target; the missile is spawned along the aim direction and
// the combat-arena steering pass guides it onto hostile combat entities. Slow
// launch + high projectile HP make it the point-defense counterplay threat.
// ---------------------------------------------------------------------------
struct MissileLauncher : Weapon {
    // tuning
    f32 reload_time;        // seconds per tube cycle
    f32 missile_speed;      // launch speed added along the aim direction (world units/s)
    f32 missile_lifetime;   // seconds before self-destruct
    f32 missile_radius;     // visual/collision radius (world units)
    f32 missile_emission;   // 0..1 radiation heat: hot engine = strongly sensor-visible
    f32 missile_hp;         // point-defense must burn through this (shell = 1.0)
    f32 cap_cost_value;     // capacitor cost per launch (member, editor-exposable)
    // runtime state
    f32 cooldown_remaining; // seconds until the next launch
    MissileLauncher(const char* name,
                    f32 reload,
                    f32 speed,
                    f32 lifetime,
                    f32 radius,
                    f32 emission,
                    f32 hp);
    void fire(bs_math::HierPos2 origin, bs_math::Vec2 direction,
              bs_math::Vec2 ship_velocity,
              ProjectileSystem* projectiles) override;
    void update(f32 dt) override;
    b8   ready() const override;
    f32  cooldown_progress() const override;
    f32  projectile_speed() const override { return missile_speed; }
    f32  cap_cost() const override { return cap_cost_value; }
};
// ---------------------------------------------------------------------------
// Effective reach: how far a shot from this weapon can actually travel before it expires
// (projectile speed x lifetime). SINGLE SOURCE OF TRUTH for "can this weapon hit something at
// distance d" -- the RTS attack order gates firing on it and the HUD range ring draws it, so what
// the player sees is exactly what the ship does. Returns 0 for a weapon with no usable stats.
// ---------------------------------------------------------------------------
f32 weapon_effective_reach(const Weapon* w);

// ---------------------------------------------------------------------------
// Helper: instances are normally built from `.weapon` defs by weapon_instantiate
// (sim/weapon_def.h). No hardcoded factories remain.
// ---------------------------------------------------------------------------
