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
// FLAK flight envelope: a MODE_FLAK shell leaves at this fraction of the def's proj speed
// and lives this long regardless of the authored lifetime. ONE site for the numbers --
// BallisticWeapon::spawn_shot flies by them and the autonomous screen
// (sim/flak_screen.cpp) engages by them, so the screen never shoots beyond its shells.
#define FLAK_SPEED_MUL  0.6f
#define FLAK_LIFETIME_S 1.4f
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
    // Operational status: TRUE = the mount is knocked out and holds fire regardless of cooldown,
    // capacitor or arc. Nothing sets it yet (there is no subsystem-damage model); it exists so
    // the fire path and the micro-selection hub agree the moment a producer lands.
    b8            disabled;
    // Which authored barrel fires next, for WeaponDef muzzles under MUZZLE_SEQUENTIAL.
    // Per INSTANCE, so two of the same gun on one hull cycle independently. Meaningless
    // for a weapon that authors no muzzles.
    u8            next_muzzle;
    Weapon(const char* n) : name(n), icon("ic-cannon"), def(nullptr), wkind(WEAPON_KIND_BALLISTIC), size(HARDPOINT_MEDIUM), damage(15.0f), owner_faction((VesselFaction)0), owner_faction_id(FACTION_PIRATE), disabled(FALSE), next_muzzle(0) {}
    virtual ~Weapon() {}
    // Put ONE projectile in the world at `origin`, along `direction` (unit or non-unit),
    // without touching the cooldown. `ship_velocity` is the owning ship's current world
    // velocity (added to the projectile). Split out of fire() so a multi-barrel salvo can
    // put one shot down each barrel on a single trigger pull; callers that want the normal
    // one-shot-plus-cooldown behaviour call fire().
    virtual void spawn_shot(bs_math::HierPos2 origin, bs_math::Vec2 direction,
                            bs_math::Vec2 ship_velocity,
                            ProjectileSystem* projectiles) = 0;
    // Start this weapon's cooldown, as if it had just fired. Pairs with spawn_shot.
    virtual void begin_cooldown() = 0;
    // One shot from `origin`, then the cooldown: spawn_shot + begin_cooldown, gated on
    // ready(). The long-standing entry point; every existing caller keeps its behaviour.
    void fire(bs_math::HierPos2 origin, bs_math::Vec2 direction,
              bs_math::Vec2 ship_velocity, ProjectileSystem* projectiles) {
        if (!projectiles || !ready()) return;
        spawn_shot(origin, direction, ship_velocity, projectiles);
        begin_cooldown();
    }
    // per-frame update (cooldowns, charge states, etc.)
    virtual void update(f32 dt) = 0;
    // TRUE if the weapon can fire right now (cooldown expired)
    virtual b8 ready() const = 0;
    // 0..1 charge / cooldown progress (for UI). 1.0 = ready, 0.0 = just fired.
    virtual f32 cooldown_progress() const = 0;
    // Full cycle time in seconds -- a ballistic's 1/fire_rate, a launcher's reload. Read-only,
    // and paired with cooldown_progress() so a caller can recover ABSOLUTE seconds-to-ready
    // rather than a fraction: (1 - progress) * duration. The charge-up VFX needs that because a
    // fixed-length anticipation window is the same 0.4 s on a 1.25 s railgun and a 9 s torpedo,
    // where the equivalent FRACTION differs by a factor of seven.
    virtual f32 cooldown_duration_s() const = 0;
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
    void spawn_shot(bs_math::HierPos2 origin, bs_math::Vec2 direction,
                    bs_math::Vec2 ship_velocity,
                    ProjectileSystem* projectiles) override;
    void begin_cooldown() override { cooldown_remaining = cooldown_duration; }
    void update(f32 dt) override;
    b8   ready() const override;
    f32  cooldown_progress() const override;
    f32  cooldown_duration_s() const override { return cooldown_duration; }
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
    void spawn_shot(bs_math::HierPos2 origin, bs_math::Vec2 direction,
                    bs_math::Vec2 ship_velocity,
                    ProjectileSystem* projectiles) override;
    void begin_cooldown() override { cooldown_remaining = reload_time; }
    void update(f32 dt) override;
    b8   ready() const override;
    f32  cooldown_progress() const override;
    f32  cooldown_duration_s() const override { return reload_time; }
    f32  projectile_speed() const override { return missile_speed; }
    f32  cap_cost() const override { return cap_cost_value; }
};
// ---------------------------------------------------------------------------
// Effective reach: how far a shot from this weapon can actually travel before it expires
// (projectile speed x lifetime). SINGLE SOURCE OF TRUTH for "can this weapon hit something at
// distance d" -- the fire gate (ship_weapon_fire_state) tests it, so a shot is never released
// at a distance it cannot physically cover. Returns 0 for a weapon with no usable stats.
//
// NOT the tactical question. For engagement GEOMETRY -- how close to fly, how far to acquire,
// what ring to draw -- use weapon_engage_range below. The two are the same number for every
// ballistic; they diverge for missiles, whose speed x life is flight ENDURANCE.
// ---------------------------------------------------------------------------
f32 weapon_effective_reach(const Weapon* w);

// Cap applied to an UNAUTHORED missile's tactical range: a card that omits `engage_range` must
// never leak flight endurance (megameters for a long-life seeker round) into engagement
// geometry. Authored cards override freely in either direction.
#define MISSILE_DEFAULT_ENGAGE_RANGE 60000.0f

// Tactical engagement range: how far away this weapon is WORTH FIGHTING from. SINGLE SOURCE OF
// TRUTH for that question -- the autopilot approaches to a fraction of it, the ROE acquisition
// envelope scales from it, avoid orders measure a threat by it, and the HUD reach ring draws
// it, so the ring and the ship's behaviour cannot disagree. Resolution: the card's authored
// `engage_range` when present; else flight reach, clamped for missiles by the default above
// (a seeker locks at a fraction of its endurance -- approaching to 85% of 4,000,000 units is
// how mounting a launcher used to read as the ship fleeing the fight). The fire gate stays on
// weapon_effective_reach: a launch at a target beyond engage range still flies its full life.
f32 weapon_engage_range(const Weapon* w);

// Reach of a MODE_FLAK shell from this weapon (proj speed * FLAK_SPEED_MUL *
// FLAK_LIFETIME_S). 0 for a null or non-ballistic weapon -- the autonomous screen's
// engagement envelope, guaranteed to match what spawn_shot actually flies.
f32 weapon_flak_reach(const Weapon* w);

// Single-pass missile lead: the aim direction from `fire_origin` toward where the target
// will be after the projectile's straight-line flight time. SINGLE SOURCE OF TRUTH for
// this solve -- extracted from the autopilot missile branch (sim/fleet.cpp, update_attack).
// The two-pass RELATIVE-velocity solves (ballistic broadside, ai_ship gunner) are a
// different problem and deliberately stay at their sites. `to_target`/`dist` are the
// already-computed origin->target vector and its length (every caller has them); a zero
// target velocity, distance or projectile speed returns `to_target` unchanged.
bs_math::Vec2 weapon_lead_dir(bs_math::HierPos2 fire_origin, bs_math::HierPos2 target_pos,
                              bs_math::Vec2 target_vel, f32 proj_speed,
                              bs_math::Vec2 to_target, f32 dist);

// ---------------------------------------------------------------------------
// Helper: instances are normally built from `.weapon` defs by weapon_instantiate
// (sim/weapon_def.h). No hardcoded factories remain.
// ---------------------------------------------------------------------------
