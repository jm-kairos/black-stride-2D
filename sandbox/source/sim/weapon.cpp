#include "sim/weapon.h"
#include <math/math_utils.h>
#include "sim/ship.h"
using namespace bs_math;
// ---------------------------------------------------------------------------
// BallisticWeapon
// ---------------------------------------------------------------------------
BallisticWeapon::BallisticWeapon(const char* name,
                                 f32 rate,
                                 f32 speed,
                                 f32 lifetime,
                                 f32 radius,
                                 f32 emission)
    : Weapon(name)
    , fire_rate(rate)
    , projectile_speed_value(speed)
    , projectile_lifetime(lifetime)
    , projectile_radius(radius)
    , projectile_emission(emission)
    , cap_cost_value(4.0f)
    , fire_mode(MODE_AP)
    , cooldown_remaining(0.0f)
{
    cooldown_duration = (fire_rate > 0.0f) ? (1.0f / fire_rate) : 1.0f;
}
void BallisticWeapon::fire(HierPos2 origin, Vec2 direction, Vec2 ship_velocity,
                           ProjectileSystem* projectiles) {
    if (!projectiles || !ready()) return;
    Vec2 dir = direction;
    f32 len = vec2_length(dir);
    if (len > 0.0001f)
        dir = vec2_scale(dir, 1.0f / len);
    // FLAK mode (Phase D): slower, short-lived proximity shells -- a defensive screen the
    // player lays manually in front of incoming ordnance. Envelope = speed * lifetime.
    const b8  flak     = (fire_mode == MODE_FLAK);
    const f32 speed    = projectile_speed_value * (flak ? 0.6f : 1.0f);
    const f32 lifetime = flak ? 1.4f : projectile_lifetime;
    Vec2 vel = vec2_add(ship_velocity, vec2_scale(dir, speed));
    // colour by faction
    bs_color col = bs_color{ 1.0f, 1.0f, 1.0f, 1.0f };
    switch (owner_faction) {
        case VESSEL_PIRATE:     col = bs_color{ 1.00f, 0.75f, 0.35f, 1.0f }; break;
        case VESSEL_FEDERATION: col = bs_color{ 1.00f, 0.75f, 0.35f, 1.0f }; break;
        case VESSEL_NEUTRAL:    col = bs_color{ 1.00f, 0.75f, 0.35f, 1.0f }; break;
        case VESSEL_STATION:    col = bs_color{ 1.00f, 0.75f, 0.35f, 1.0f }; break;
        case VESSEL_DERELICT:   col = bs_color{ 1.00f, 0.75f, 0.35f, 1.0f }; break;
        default: break;
    }
    if (flak) col = bs_color{ 0.82f, 0.85f, 0.88f, 1.0f };   // pale burst-grey tracer
    projectiles->spawn(origin, vel, lifetime,
                       projectile_radius, col, owner_faction, owner_faction_id,
                       projectile_emission, 1.0f, flak ? PROJ_FLAK : PROJ_SHELL);
    cooldown_remaining = cooldown_duration;
}
void BallisticWeapon::update(f32 dt) {
    if (cooldown_remaining > 0.0f) {
        cooldown_remaining -= dt;
        if (cooldown_remaining < 0.0f) cooldown_remaining = 0.0f;
    }
}
b8 BallisticWeapon::ready() const {
    return cooldown_remaining <= 0.0f;
}
f32 BallisticWeapon::cooldown_progress() const {
    if (cooldown_duration <= 0.0f) return 0.0f;
    f32 t = 1.0f - (cooldown_remaining / cooldown_duration);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t;
}
// ---------------------------------------------------------------------------
// MissileLauncher
// ---------------------------------------------------------------------------
MissileLauncher::MissileLauncher(const char* name,
                                 f32 reload,
                                 f32 speed,
                                 f32 lifetime,
                                 f32 radius,
                                 f32 emission,
                                 f32 hp)
    : Weapon(name)
    , reload_time(reload)
    , missile_speed(speed)
    , missile_lifetime(lifetime)
    , missile_radius(radius)
    , missile_emission(emission)
    , missile_hp(hp)
    , cap_cost_value(25.0f)
    , cooldown_remaining(0.0f)
{
    icon  = "ic-missile";
    wkind = WEAPON_KIND_MISSILE;
}
void MissileLauncher::fire(HierPos2 origin, Vec2 direction, Vec2 ship_velocity,
                           ProjectileSystem* projectiles) {
    if (!projectiles || !ready()) return;
    Vec2 dir = direction;
    f32 len = vec2_length(dir);
    if (len > 0.0001f)
        dir = vec2_scale(dir, 1.0f / len);
    Vec2 vel = vec2_add(ship_velocity, vec2_scale(dir, missile_speed));
    // Exhaust-orange tint distinguishes missiles from cannon shells until bespoke art lands.
    bs_color col = bs_color{ 1.00f, 0.45f, 0.20f, 1.0f };
    projectiles->spawn_missile(origin, vel, missile_lifetime, missile_radius, col,
                               owner_faction, owner_faction_id, missile_emission, missile_hp);
    cooldown_remaining = reload_time;
}
void MissileLauncher::update(f32 dt) {
    if (cooldown_remaining > 0.0f) {
        cooldown_remaining -= dt;
        if (cooldown_remaining < 0.0f) cooldown_remaining = 0.0f;
    }
}
b8 MissileLauncher::ready() const {
    return cooldown_remaining <= 0.0f;
}
f32 MissileLauncher::cooldown_progress() const {
    if (reload_time <= 0.0f) return 0.0f;
    f32 t = 1.0f - (cooldown_remaining / reload_time);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t;
}
// ---------------------------------------------------------------------------
// Factory helpers
// ---------------------------------------------------------------------------
Weapon* weapon_create_ballistic_cannon(VesselFaction owner) {
    BallisticWeapon* w = new BallisticWeapon(
        "Ballistic Cannon",
        5.0f,      // 5 shots/sec
        12000.0f,   // 12000 units/s
        20.0f,     // 20 second lifetime
        4.0f,      // 4 unit radius
        0.6f       // 0..1 radiation emission
    );
    w->owner_faction = owner;
    return w;
}
Weapon* weapon_create_missile_launcher(VesselFaction owner) {
    MissileLauncher* w = new MissileLauncher(
        "Missile Launcher",
        4.0f,      // 4 s reload per tube
        3000.0f,   // slow launch (cannon shell = 12000; head-on outrun is possible)
        12.0f,     // 12 s lifetime, then self-destruct
        6.0f,      // slightly larger than a shell (visual distinction + PD hit ease)
        0.9f,      // hot engine: strongly visible to radiation sensors (counterplay)
        3.5f       // ~0.3 s of standard PD dwell to kill (shell = 1.0)
    );
    w->owner_faction = owner;
    return w;
}
