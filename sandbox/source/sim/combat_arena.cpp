#include "sim/combat_arena.h"
#include "game.h"            // full game_state (ship.h / projectile.h / fleet.h via the cascade)
#include "sim/action_log.h"  // action_log_push
#include "core/geom2d.h"     // point_in_polygon
#include <math.h>            // sqrtf

using namespace bs_math;

void combat_arena_init(game_state* s) {
    // ---- Projectile system -----------------------------------------------------------------
    s->projectiles.init();

    // ---- Combat entities -------------------------------------------------------------------
    s->combat_entity_count = 0;
    for (i32 i = 0; i < MAX_COMBAT_ENTITIES; ++i) {
        s->combat_entities[i].active = FALSE;
        s->combat_entities[i].velocity = Vec2{ 0.0f, 0.0f };
        s->combat_entities[i].heat_history_count = 0;
    }

    // Register enemy ship as a combat entity.
    {
        CombatEntity* ce = &s->combat_entities[0];
        ce->active   = TRUE;
        ce->position = s->fleet_state.enemy_ship.origin;
        ce->velocity = Vec2{ 0.0f, 0.0f };
        ce->radius   = ship_bounding_radius(&s->fleet_state.enemy_ship);
        ce->faction  = s->fleet_state.enemy_ship.faction;
        ce->hp       = 100.0f;
        ce->ship     = &s->fleet_state.enemy_ship;
        ce->tint     = bs_color{ 1.0f, 0.3f, 0.3f, 1.0f };
        ce->radiation_emission = s->fleet_state.enemy_ship.radiation_emission;
        ce->is_drone = FALSE;
        s->combat_entity_count = 1;
    }

    // Register every fleet ship as a combat entity so enemy fire can hit them.
    for (i32 i = 0; i < s->fleet_state.fleet.count() && s->combat_entity_count < MAX_COMBAT_ENTITIES; ++i) {
        FleetShip& fs = s->fleet_state.fleet.at(i);
        CombatEntity* ce = &s->combat_entities[s->combat_entity_count++];
        ce->active   = TRUE;
        ce->position = fs.ship.origin;
        ce->velocity = fs.flight.velocity;
        ce->radius   = ship_bounding_radius(&fs.ship);
        ce->faction  = fs.ship.faction;
        ce->hp       = 100.0f;
        ce->ship     = &fs.ship;
        ce->tint     = bs_color{ 0.3f, 0.8f, 1.0f, 1.0f };
        ce->radiation_emission = fs.ship.radiation_emission;
        ce->is_drone = TRUE;
    }

    // ---- Sensor / heat-signature / encounter tunables --------------------------------------
    s->ship_sensor_range      = 30000.0f;
    s->out_sensor_fx.init();
    s->show_metaball_ui       = FALSE;

    s->base_detection_radius  = 5000.0f;
    s->heat_signature_radius = 1000.0f;

    s->heat_palette = 0;
    s->heat_color_low = bs_color{0.0f, 0.0f, 0.0f, 1.0f};
    s->heat_color_high = bs_color{1.0f, 1.0f, 1.0f, 1.0f};
    s->heat_color_falloff_power = 1.0f;

    s->metaball_radius_factor = 2.0f;
    s->metaball_threshold     = 1.0f;
    s->heat_map_intensity     = 0.5f;
    s->heat_tail_length       = 8.0f;
    s->heat_tail_fade         = 2.0f;
    s->heat_warp_strength     = 30.0f;
    s->heat_map_venn_sharpness = 0.0f;

    s->encounter_active       = FALSE;
    s->encounter_was_active   = FALSE;
    s->encounter_can_retrigger = TRUE;
}

void combat_arena_update_encounter(game_state* s) {
    // ---- Encounter detection: blob merge ------------------------------------------------
    f32 threshold = s->metaball_threshold;
    if (threshold < 1.0e-4f) threshold = 1.0e-4f;
    f32 radius = s->base_detection_radius * s->metaball_radius_factor;
    f32 reach = radius / sqrtf(threshold);
    Vec2 delta = hierpos_diff(&s->player_ship().origin, &s->fleet_state.enemy_ship.origin, BS_HIERPOS_CELL_SIZE);
    f32 dist   = vec2_length(delta);
    b8 enemy_detected = (dist < reach);
    if (enemy_detected && !s->encounter_active && s->encounter_can_retrigger) {
        s->encounter_active = TRUE;
        s->encounter_can_retrigger = FALSE;
        s->time_scale = 0.0f; // pause on encounter
        action_log_push(s, "Encounter detected! Enemy ship nearby.");
    }
    if (!enemy_detected && !s->encounter_active) {
        // Ships separated far enough -- allow re-trigger next approach.
        s->encounter_can_retrigger = TRUE;
    }
    s->encounter_was_active = s->encounter_active;
}

// Hardcoded demo: make the enemy ship orbit the player flagship.
void combat_arena_update_enemy_orbit(game_state* s, f32 dt) {
    if (!s) return;

    const f32 ORBIT_RADIUS = 15000.0f;
    const f32 ORBIT_ANGULAR_SPEED = 0.3f;

    HierPos2 flag = s->fleet_state.fleet.flagship().ship.origin;

    s->fleet_state.enemy_orbit_phase += ORBIT_ANGULAR_SPEED * dt;
    while (s->fleet_state.enemy_orbit_phase > 2.0f * BS_PI) s->fleet_state.enemy_orbit_phase -= 2.0f * BS_PI;
    while (s->fleet_state.enemy_orbit_phase < 0.0f) s->fleet_state.enemy_orbit_phase += 2.0f * BS_PI;

    Vec2 offset = vec2_rotate(Vec2{ ORBIT_RADIUS, 0.0f }, s->fleet_state.enemy_orbit_phase);
    s->fleet_state.enemy_ship.origin = hierpos_add_vec2(&flag, offset);
    s->fleet_state.enemy_ship.angle  = s->fleet_state.enemy_orbit_phase + BS_PI * 0.5f;
}

void combat_arena_sync_entities(game_state* s, f32 sim_dt) {
    // ---- Sync combat entity positions / velocities from their ships --------------------
    for (i32 i = 0; i < s->combat_entity_count; ++i) {
        CombatEntity* ce = &s->combat_entities[i];
        if (!ce->active || !ce->ship) continue;
        HierPos2 prev_hp = ce->position;
        ce->position = ce->ship->origin;
        ShipFlight* fl = s->fleet_state.fleet.flight_for_ship(ce->ship);
        if (fl) {
            ce->velocity = fl->velocity;
        } else if (sim_dt > 0.0001f) {
            // Derive velocity from position change for ships that don't have a flight state.
            ce->velocity = vec2_scale(hierpos_diff(&ce->position, &prev_hp, BS_HIERPOS_CELL_SIZE), 1.0f / sim_dt);
        }
        // Drone heat signature scales with speed: 0.005f when halted, up to 0.05f at full speed.
        if (ce->is_drone) {
            f32 speed_ratio = clampf(vec2_length(ce->velocity) / SHIP_MAX_SPEED, 0.0f, 1.0f);
            ce->radiation_emission = 0.005f + (0.05f - 0.005f) * speed_ratio;
        }
        // Record position history for the heat-map trail (only for radiating entities).
        if (ce->radiation_emission > 0.0f) {
            for (i32 h = HEAT_HISTORY_LEN - 1; h > 0; --h) {
                ce->heat_history[h] = ce->heat_history[h - 1];
            }
            ce->heat_history[0] = ce->position;
            ce->heat_history_count = (ce->heat_history_count + 1 < HEAT_HISTORY_LEN) ? ce->heat_history_count + 1 : HEAT_HISTORY_LEN;
        } else {
            ce->heat_history_count = 0;
        }
    }
}

void combat_arena_update_projectiles(game_state* s, f32 sim_dt) {
    // ---- Update projectiles -------------------------------------------------------------
    s->profiler.begin(PROF_PROJECTILES);
    s->projectiles.update(sim_dt);

    // ---- Projectile vs combat entity collision ------------------------------------------
    for (i32 pi = 0; pi < MAX_PROJECTILES; ++pi) {
        Projectile* p = &s->projectiles.pool[pi];
        if (!p->active) continue;
        for (i32 ci = 0; ci < s->combat_entity_count; ++ci) {
            CombatEntity* ce = &s->combat_entities[ci];
            if (!ce->active) continue;
            if (ce->faction == p->owner) continue; // don't hit own faction
            Vec2 to_ce = hierpos_diff(&ce->position, &p->position, BS_HIERPOS_CELL_SIZE);
            f32 dist2 = to_ce.x * to_ce.x + to_ce.y * to_ce.y;
            f32 rr = ce->radius + p->radius;
            if (dist2 < rr * rr) {
                b8 hit = TRUE;
                if (ce->ship) {
                    Vec2 corners[SHIP_MAX_COLLIDER_VERTS];
                    if (ship_collider_corners(ce->ship, corners)) {
                        // corners are ship-origin-relative; test the projectile in that same frame.
                        Vec2 p_rel = hierpos_diff(&p->position, &ce->ship->origin, BS_HIERPOS_CELL_SIZE);
                        hit = point_in_polygon(p_rel, corners, ce->ship->collider_count);
                    }
                }
                if (hit) {
                    p->active = FALSE;
                    --s->projectiles.count;
                    break; // projectile can only hit one entity
                }
            }
        }
    }
    s->profiler.end(PROF_PROJECTILES);
}
