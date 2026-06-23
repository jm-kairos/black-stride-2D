#include "weapon.h"
#include <math/math_utils.h>
#include <ship.h>
using namespace bs_math;
// ---------------------------------------------------------------------------
// BallisticWeapon
// ---------------------------------------------------------------------------
BallisticWeapon::BallisticWeapon(const char* name,
                                 f32 rate,
                                 f32 speed,
                                 f32 lifetime,
                                 f32 radius)
    : Weapon(name)
    , fire_rate(rate)
    , projectile_speed_value(speed)
    , projectile_lifetime(lifetime)
    , projectile_radius(radius)
    , cooldown_remaining(0.0f)
{
    cooldown_duration = (fire_rate > 0.0f) ? (1.0f / fire_rate) : 1.0f;
}
void BallisticWeapon::fire(Vec2 origin, Vec2 direction, Vec2 ship_velocity,
                           ProjectileSystem* projectiles) {
    if (!projectiles || !ready()) return;
    Vec2 dir = direction;
    f32 len = vec2_length(dir);
    if (len > 0.0001f)
        dir = vec2_scale(dir, 1.0f / len);
    Vec2 vel = vec2_add(ship_velocity, vec2_scale(dir, projectile_speed_value));
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
    projectiles->spawn(origin, vel, projectile_lifetime,
                       projectile_radius, col, owner_faction);
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
// Factory helpers
// ---------------------------------------------------------------------------
Weapon* weapon_create_ballistic_cannon(VesselFaction owner) {
    BallisticWeapon* w = new BallisticWeapon(
        "Ballistic Cannon",
        5.0f,      // 5 shots/sec
        1200.0f,   // 1200 units/s
        2.0f,      // 2 second lifetime
        4.0f       // 4 unit radius
    );
    w->owner_faction = owner;
    return w;
}
