#include "sim/sensor_system.h"
#include "game.h" // game_state, Fleet, Ship, ProjectileSystem, MAX_PROJECTILES

using namespace bs_math;

f32 sensor_reading(const SensorSuite* suite, f32 dist) {
    if (!suite) return 0.0f;
    f32 denom = suite->layer2_radius - suite->layer1_radius;
    if (denom < 1.0e-4f) denom = 1.0e-4f; // guard against L2 <= L1 misconfiguration
    return clampf((suite->layer2_radius - dist) / denom, 0.0f, 1.0f);
}

i32 sensor_gather_hostile_contacts(const game_state* s, SensorContact* out, i32 max_out) {
    if (!s || !out || max_out <= 0) return 0;

    const Fleet& fleet = s->fleet_state.fleet;

    i32 n = 0;
    for (i32 i = 0; i < MAX_PROJECTILES && n < max_out; ++i) {
        const Projectile& p = s->projectiles.pool[i];
        if (!p.active) continue;
        if (p.faction_id == FACTION_PLAYER) continue; // only non-player projectiles are contacts

        // Fleet-wide detection: keep the strongest single reading over all friendly ships.
        f32 best = 0.0f;
        for (i32 j = 0; j < fleet.count(); ++j) {
            const Ship& ship = fleet.at(j).ship;
            f32 d = vec2_length(hierpos_diff(&p.position, &ship.origin, BS_HIERPOS_CELL_SIZE));
            f32 t = sensor_reading(&ship.sensors, d);
            if (t > best) best = t;
        }
        // Layer 2 is UNBOUNDED: every hostile projectile is a contact regardless of range. The
        // confidence (best) may be 0 far beyond Layer 2 -- the overlay dims those distant returns
        // with distance -- but they are still detected and tracked.
        SensorContact& c = out[n++];
        c.position   = p.position;
        c.velocity   = p.velocity;
        c.confidence = best;
        c.owner      = p.owner;
    }
    return n;
}
