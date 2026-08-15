#include "sim/combat_arena.h"
#include "game.h"            // full game_state (ship.h / projectile.h / fleet.h via the cascade)
#include "sim/weapon.h"          // Weapon::fire / ready / projectile_speed
#include "sim/galaxy_history.h" // galaxy_history_faction_is_hostile (Feature B per-entity stance)
#include "sim/ai_ship.h"      // ai_ship_damage (General Ship AI: NPC combat entities)
#include "sim/action_log.h"  // action_log_push
#include "core/geom2d.h"     // point_in_polygon
#include <math.h>            // sqrtf

using namespace bs_math;

void combat_arena_init(game_state* s) {
    // ---- Projectile system -----------------------------------------------------------------
    s->projectiles.init();

    // ---- Projectile VFX ring (cosmetic) ----------------------------------------------------
    // Wired ONCE here, not per frame. The pool is a plain game_state member, so the pointer
    // stays valid for the process lifetime. Set it to nullptr to disable every launch and
    // termination effect without touching a fire site.
    s->projectile_fx.init();
    s->projectiles.fx = &s->projectile_fx;

    // ---- Combat entities -------------------------------------------------------------------
    s->combat_entity_count = 0;
    for (i32 i = 0; i < MAX_COMBAT_ENTITIES; ++i) {
        s->combat_entities[i].active = FALSE;
        s->combat_entities[i].velocity = Vec2{ 0.0f, 0.0f };
        s->combat_entities[i].heat_history_count = 0;
    }

    // Register the enemy ship + every ACTIVE fleet ship as combat entities. Recomputes
    // npc_combat_base so the NPC window that ai_ships_register_combat appends lands right after.
    combat_arena_rebuild_player_entities(s);

    // ---- Sensor / heat-signature / encounter tunables --------------------------------------
    // Enemy-hull render range: matched to the player's Layer 1 identification radius so a contact
    // resolves into a real sprite exactly when the sensor suite says it is identified.
    s->ship_sensor_range      = SENSOR_LAYER1_RADIUS;
    s->out_sensor_fx.init();
    s->show_metaball_ui       = FALSE;
    s->show_sensor_layers     = FALSE;

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

// Rebuild the persistent (non-NPC) combat-entity window: slot 0 is the enemy ship, slots
// 1..fleet.count() mirror the ACTIVE fleet ships. Sets npc_combat_base to the slot right after
// so the NPC agents (appended each frame by ai_ships_register_combat) start there. Call whenever
// the active fleet count changes (e.g. the multi-ship editor toggle) so the windows stay packed.
void combat_arena_rebuild_player_entities(game_state* s) {
    if (!s) return;
    // Register enemy ship as a combat entity (slot 0).
    {
        CombatEntity* ce = &s->combat_entities[0];
        ce->active   = TRUE;
        ce->position = s->fleet_state.enemy_ship.origin;
        ce->velocity = Vec2{ 0.0f, 0.0f };
        ce->radius   = ship_bounding_radius(&s->fleet_state.enemy_ship);
        ce->faction  = s->fleet_state.enemy_ship.faction;
        ce->faction_id = s->fleet_state.enemy_ship.faction_id;
        ce->hp       = s->fleet_state.enemy_ship.hull_max_hp;   // card stat (`hull` line; 100 default)
        ce->ship     = &s->fleet_state.enemy_ship;
        ce->tint     = bs_color{ 1.0f, 0.3f, 0.3f, 1.0f };
        ce->radiation_emission = s->fleet_state.enemy_ship.radiation_emission;
        ce->is_drone = FALSE;
    }
    s->combat_entity_count = 1;

    // Register every ACTIVE fleet ship as a combat entity so enemy fire can hit them.
    for (i32 i = 0; i < s->fleet_state.fleet.count() && s->combat_entity_count < MAX_COMBAT_ENTITIES; ++i) {
        FleetShip& fs = s->fleet_state.fleet.at(i);
        CombatEntity* ce = &s->combat_entities[s->combat_entity_count++];
        ce->active   = TRUE;
        ce->position = fs.ship.origin;
        ce->velocity = fs.flight.velocity;
        ce->radius   = ship_bounding_radius(&fs.ship);
        ce->faction  = fs.ship.faction;
        ce->faction_id = fs.ship.faction_id;
        ce->hp       = fs.ship.hull_max_hp;   // card stat (`hull` line; 100 default)
        ce->ship     = &fs.ship;
        ce->tint     = bs_color{ 0.3f, 0.8f, 1.0f, 1.0f };
        ce->radiation_emission = fs.ship.radiation_emission;
        ce->is_drone = TRUE;
    }
    // The NPC window begins right after the persistent player + enemy slots.
    s->npc_combat_base = s->combat_entity_count;
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

// Static enemy AI: the ship never translates. It rotates to track the flagship (turret) and fires
// its weapon periodically while the flagship is inside ENEMY_DETECTOR_RADIUS. The weapon cooldown
// (ticked in game_update) gives the periodic cadence.
void combat_arena_update_enemy_ai(game_state* s, f32 dt) {
    if (!s) return;

    Ship* sh = &s->fleet_state.enemy_ship;
    FleetShip& flag_fs = s->fleet_state.fleet.flagship();
    Ship* flag = &flag_fs.ship;

    // Vector from the enemy to the flagship (world units).
    Vec2 to_target = hierpos_diff(&flag->origin, &sh->origin, BS_HIERPOS_CELL_SIZE);
    f32  dist      = vec2_length(to_target);

    // Firing range is unbounded for now: only bail on a coincident target. Position is never
    // changed -> the ship remains static.
    if (dist < 0.0001f) return;

    // Rotate the nose toward the flagship (turret tracking only; no movement).
    f32 desired_angle = atan2f(-to_target.x, to_target.y);
    f32 angle_diff    = desired_angle - sh->angle;
    while (angle_diff >  BS_PI) angle_diff -= 2.0f * BS_PI;
    while (angle_diff < -BS_PI) angle_diff += 2.0f * BS_PI;
    f32 max_rot = SHIP_MAX_TURN * dt;
    sh->angle += clampf(angle_diff, -max_rot, max_rot);

    // Mounted weapon turrets track the flagship even while holding fire (visual traverse).
    for (i32 hpi = 0; hpi < sh->hardpoint_count; ++hpi)
        if (sh->mounts[hpi]) ship_turret_aim_at(sh, hpi, to_target);

    // Feature B: resolve hostility per-entity from this hull's own faction_id (folds transitive
    // diplomacy: an ally's enemies read hostile). Wild space / pirates always engage; the player's
    // own faction never does. Non-hostile patrols just track and watch - they hold fire.
    if (!galaxy_history_faction_is_hostile(s, sh->faction_id)) return;

    // Fire only when roughly aligned and the weapon is off cooldown.
    const f32 ENEMY_FIRE_FACE_ANGLE = 0.20f; // rad
    if (fabsf(angle_diff) > ENEMY_FIRE_FACE_ANGLE) return;
    // Fire whichever mounted weapon bears on the flagship (active weapon first): each shot
    // leaves from its own hardpoint, honoring the slot's traverse arc.
    i32 whp   = ship_select_bearing_weapon(sh, to_target);
    Weapon* w = (whp >= 0) ? sh->mounts[whp] : nullptr;
    if (!w) return;

    HierPos2 fire_origin = ship_hardpoint_fire_origin(sh, whp);
    Vec2 aim_dir = to_target;
    // Lead the flagship using its velocity so shots can connect while it maneuvers.
    Vec2 target_vel = flag_fs.flight.velocity;
    if ((target_vel.x != 0.0f || target_vel.y != 0.0f)) {
        f32 proj_speed = w->projectile_speed();
        if (proj_speed > 0.0001f) {
            f32 lead_time = dist / proj_speed;
            HierPos2 lead_pos = hierpos_add_vec2(&flag->origin, vec2_scale(target_vel, lead_time));
            aim_dir = hierpos_diff(&lead_pos, &fire_origin);
        }
    }
    // Enemy is static, so its own muzzle velocity contribution is zero.
    w->owner_faction_id = sh->faction_id;   // stamp attacker faction (the patrol hull's live civ)
    if (w->ready() && ship_try_spend_cap(sh, w->cap_cost()))   // capacitor gate
        ship_hardpoint_fire(sh, whp, aim_dir, Vec2{ 0.0f, 0.0f }, &s->projectiles);

    // Missile launchers fire independently of the bearing-weapon pick: any mounted, ready
    // launcher looses a seeker at the flagship whenever the hull is roughly aligned. Missiles
    // guide themselves after launch (combat_arena_steer_missiles), so no lead is needed.
    for (i32 hpi = 0; hpi < sh->hardpoint_count; ++hpi) {
        Weapon* ml = sh->mounts[hpi];
        if (!ml || ml->wkind != WEAPON_KIND_MISSILE || ml == w || !ml->ready()) continue;
        if (!ship_try_spend_cap(sh, ml->cap_cost())) continue;   // capacitor gate
        HierPos2 ml_origin = ship_hardpoint_fire_origin(sh, hpi);
        Vec2 ml_dir = hierpos_diff(&flag->origin, &ml_origin);
        ml->owner_faction_id = sh->faction_id;
        ship_hardpoint_fire(sh, hpi, ml_dir, Vec2{ 0.0f, 0.0f }, &s->projectiles);
    }
}

void combat_arena_sync_entities(game_state* s, f32 sim_dt) {
    // ---- Sync combat entity positions / velocities from their ships --------------------
    for (i32 i = 0; i < s->combat_entity_count; ++i) {
        CombatEntity* ce = &s->combat_entities[i];
        if (!ce->active || !ce->ship) continue;
        ce->faction_id = ce->ship->faction_id; // keep the entity's faction in sync with its ship
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
    // ---- Missile guidance (fire-and-seek) -------------------------------------------------
    // Steer every live PROJ_MISSILE toward the nearest hostile combat entity inside its
    // seeker cone/range BEFORE positions advance. Runs after point_defense_update in the
    // tick order, so PD kills this frame never steer. Breaking the cone (a hard turn behind
    // the missile) drops it to dumbfire until lifetime expiry -- that is the maneuver
    // counterplay. Flight model lives in s->missile_tuning (editor-tunable, single type).
    s->profiler.begin(PROF_PROJECTILES);
    {
        const f32 cone_cos = cosf(s->missile_tuning.seeker_cone_deg * (3.14159265f / 180.0f));
        for (i32 pi = 0; pi < MAX_PROJECTILES; ++pi) {
            Projectile* p = &s->projectiles.pool[pi];
            if (!p->active || p->kind != PROJ_MISSILE) continue;

            f32 speed = vec2_length(p->velocity);
            if (speed < 1.0e-3f) speed = 1.0e-3f;
            Vec2 heading = vec2_scale(p->velocity, 1.0f / speed);

            // Nearest hostile combat entity inside the seeker cone.
            i32 best   = -1;
            f32 best_d = s->missile_tuning.seeker_range;
            for (i32 ci = 0; ci < s->combat_entity_count; ++ci) {
                const CombatEntity* ce = &s->combat_entities[ci];
                if (!ce->active) continue;
                if (ce->faction_id == p->faction_id) continue;
                Vec2 to = hierpos_diff(&ce->position, &p->position, BS_HIERPOS_CELL_SIZE);
                f32 d = vec2_length(to);
                if (d <= 1.0e-3f || d > best_d) continue;
                if ((to.x * heading.x + to.y * heading.y) / d < cone_cos) continue;
                best_d = d;
                best   = ci;
            }

            if (best >= 0) {
                // Rotate the heading toward the target, clamped by turn_rate * dt.
                Vec2 to  = hierpos_diff(&s->combat_entities[best].position, &p->position,
                                        BS_HIERPOS_CELL_SIZE);
                f32 want = atan2f(to.y, to.x);
                f32 cur  = atan2f(heading.y, heading.x);
                f32 delta = want - cur;
                while (delta >  3.14159265f) delta -= 6.28318531f;
                while (delta < -3.14159265f) delta += 6.28318531f;
                f32 max_step = s->missile_tuning.turn_rate * sim_dt;
                if (delta >  max_step) delta =  max_step;
                if (delta < -max_step) delta = -max_step;
                cur += delta;
                heading = Vec2{ cosf(cur), sinf(cur) };
            }

            // Thrust along the (possibly unchanged) heading up to the speed clamp. The clamp is
            // the SEEKER'S OWN, stamped from its launcher's proj_speed, so a .weapon file governs
            // how fast its missiles fly and therefore how far they reach within proj_life. It was
            // previously a single global, which silently overrode every launcher's proj_speed the
            // frame after launch: editing proj_speed moved the displayed range ring but not the
            // missile, so the ring and reality disagreed.
            f32 cap = (p->max_speed > 0.0f) ? p->max_speed : s->missile_tuning.max_speed;

            speed += s->missile_tuning.accel * sim_dt;

            if (speed > cap) speed = cap;

            p->velocity = vec2_scale(heading, speed);
        }
    }

    // ---- Update projectiles -------------------------------------------------------------
    s->projectiles.update(sim_dt);

    // ---- Flak fuse + burst (Phase D) ------------------------------------------------------
    // Each live PROJ_FLAK shell detonates when hostile ordnance enters its fuse radius,
    // applying linear-falloff HP damage to ALL hostile projectiles inside the burst radius
    // (missiles and shells alike -- a manually-laid defensive wall). Flak never touches
    // ships: it is skipped in the entity-collision loop below.
    {
        const f32 fuse  = s->flak_tuning.fuse_radius;
        const f32 burst = s->flak_tuning.burst_radius;
        for (i32 fi = 0; fi < MAX_PROJECTILES; ++fi) {
            Projectile* fp = &s->projectiles.pool[fi];
            if (!fp->active || fp->kind != PROJ_FLAK) continue;
            // Fuse scan: any hostile ordnance close enough?
            b8 trigger = FALSE;
            for (i32 pi = 0; pi < MAX_PROJECTILES; ++pi) {
                const Projectile* p = &s->projectiles.pool[pi];
                if (!p->active || pi == fi) continue;
                if (p->faction_id == fp->faction_id) continue;
                Vec2 d = hierpos_diff(&p->position, &fp->position, BS_HIERPOS_CELL_SIZE);
                if (d.x * d.x + d.y * d.y <= fuse * fuse) { trigger = TRUE; break; }
            }
            if (!trigger) continue;
            // Burst: falloff damage to every hostile projectile in radius.
            i32 kills = 0;
            for (i32 pi = 0; pi < MAX_PROJECTILES; ++pi) {
                Projectile* p = &s->projectiles.pool[pi];
                if (!p->active || pi == fi) continue;
                if (p->faction_id == fp->faction_id) continue;
                Vec2 dv = hierpos_diff(&p->position, &fp->position, BS_HIERPOS_CELL_SIZE);
                f32 d = sqrtf(dv.x * dv.x + dv.y * dv.y);
                if (d > burst) continue;
                p->hp -= s->flak_tuning.burst_damage * (1.0f - d / burst);
                if (p->hp <= 0.0f) {
                    // Ordnance killed in flight reads the same whether flak or a PD beam did
                    // it -- from the player's side both are "the screen worked".
                    s->projectiles.retire(pi, PFX_INTERCEPT);
                    ++kills;
                }
            }
            // Attribution (Phase E): the player's manually-laid screen reports its work.
            if (kills > 0 && fp->faction_id == FACTION_PLAYER)
                action_log_push(s, "Flak burst: %d ordnance destroyed", kills);
            // The shell is consumed by its own burst. Drawn at the BURST radius rather than the
            // shell's own 3-unit radius, so the airburst covers the volume that actually took
            // ordnance down -- the effect doubles as the readout for where the screen reaches.
            s->projectiles.retire(fi, PFX_BURST, s->flak_tuning.burst_radius);
        }
    }

    // ---- Projectile vs combat entity collision ------------------------------------------
    for (i32 pi = 0; pi < MAX_PROJECTILES; ++pi) {
        Projectile* p = &s->projectiles.pool[pi];
        if (!p->active) continue;
        if (p->kind == PROJ_FLAK) continue;   // flak is purely anti-ordnance: never hits hulls
        for (i32 ci = 0; ci < s->combat_entity_count; ++ci) {
            CombatEntity* ce = &s->combat_entities[ci];
            if (!ce->active) continue;
            if (ce->faction_id == p->faction_id) continue; // no friendly fire within a faction
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
                    // General Ship AI: NPC agents take damage; the projectile's faction id
                    // attributes the kill (player kills raid the civ, NPC kills don't).
                    if (ce->is_npc) ai_ship_damage(s, ce->npc_index, p->dmg, p->faction_id);
                    // Return-fire attribution: a hostile round landing on a FLEET hull marks
                    // its faction as an aggressor (decaying entry on the Fleet), which is what
                    // ROE_RETURN_FIRE ships are authorised to self-acquire against. Faction-
                    // level rather than Ship* so a despawned shooter can never dangle.
                    if (ce->faction_id == FACTION_PLAYER && p->faction_id != FACTION_PLAYER)
                        s->fleet_state.fleet.note_aggression(p->faction_id);
                    // Attributed missile-hit feedback (Phase E): when a missile lands on a
                    // player fleet ship, say WHY the defenses missed it -- read from the hit
                    // ship's own PD state at impact time. Disabled/unmounted reads as holding.
                    if (p->kind == PROJ_MISSILE && ce->is_drone && ce->ship) {
                        const DefenseLaser& pdl = ce->ship->point_defense;
                        const char* cause;
                        if (!pdl.enabled || ce->ship->point_defense_mount < 0 || pdl.stance == PD_HOLD)
                            cause = "PD holding";
                        else if (ce->ship->cap_current < pdl.reserve_floor * ce->ship->cap_max)
                            cause = "capacitor dry";
                        else
                            cause = "PD saturated";
                        action_log_push(s, "MISSILE HIT -- %s", cause);
                    }
                    s->projectiles.retire(pi, PFX_IMPACT);
                    break; // projectile can only hit one entity
                }
            }
        }
    }
    s->profiler.end(PROF_PROJECTILES);
}
