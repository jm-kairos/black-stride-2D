#include "sim/ai_ship.h"

#include "game.h"                 // full game_state (Ship, ShipFlight, projectiles, galaxy)

#include "sim/galaxy_map.h"       // galaxy_nearest_node

#include "sim/galaxy_history.h"   // owner_at_node / faction_is_hostile / player_raid

#include "sim/ship_mission.h"     // ship_mission_notify_destroyed (macro <-> local handoff)

#include "ship_visual.h"          // ship_visual_resolve_textures

#include "sim/steering.h"         // shared steering (arrive/seek/standoff + apply)

#include "sim/discovery.h"        // discovery_npc_is_known (single-ship discovery system)

#include <core/logger.h>          // BS_LOG_ERROR

#include <math.h>



using namespace bs_math;

using GalaxyState = game_state::GalaxyState;   // nested alias for the mission-handoff helpers



// =====================================================================================

// Per-archetype tuning. Distances scaled to the LOCAL combat scale (player SHIP_MAX_SPEED == 800

// units/s), not the galaxy scale. Phase A only exercises ARCHETYPE_PATROL; the others are seeded

// with sensible defaults and come alive in Phase C.

// =====================================================================================

static const AiProfile g_profiles[ARCHETYPE_COUNT] = {

    // max_spd accel turn  sensor  engage standoff minR  patSpd patRad aggr flee  leash  fireT  states

    {  800.f, 500.f, 3.0f, 60000.f, 45000.f, 12000.f, 1500.f, 420.f, 12000.f, 0.85f, 0.25f, 80000.f, 1.5f, 0xFFFF }, // PATROL

    {  820.f, 560.f, 3.2f, 70000.f, 50000.f, 14000.f, 1500.f, 500.f, 14000.f, 1.00f, 0.15f, 90000.f, 1.2f, 0xFFFF }, // WARSHIP

    {  950.f, 640.f, 3.6f, 65000.f, 40000.f,  9000.f, 1200.f, 650.f, 12000.f, 1.00f, 0.30f, 90000.f, 1.4f, 0xFFFF }, // INTERCEPTOR

    {  760.f, 420.f, 2.6f, 55000.f, 20000.f, 18000.f, 2000.f, 520.f, 16000.f, 0.05f, 0.90f, 70000.f, 3.0f, 0xFFFF }, // TRADER

    {  900.f, 560.f, 3.4f, 90000.f, 15000.f, 30000.f, 2000.f, 560.f, 20000.f, 0.10f, 0.80f, 90000.f, 3.0f, 0xFFFF }, // SCOUT

    {  840.f, 560.f, 3.2f, 70000.f, 48000.f, 12000.f, 1400.f, 520.f, 14000.f, 1.00f, 0.20f, 99999.f, 1.2f, 0xFFFF }, // PIRATE

    {  600.f, 380.f, 2.4f, 45000.f,     0.f,     0.f,    0.f, 300.f,  9000.f, 0.00f, 0.90f, 60000.f, 3.0f, 0xFFFF }, // MINER

};



const AiProfile& ai_profile(u8 archetype) {

    if (archetype >= ARCHETYPE_COUNT) archetype = ARCHETYPE_PATROL;

    return g_profiles[archetype];

}



// ---- Deterministic-ish per-agent RNG (splitmix64) -----------------------------------------

static inline u64 sm64(u64& st) {

    u64 z = (st += 0x9e3779b97f4a7c15ull);

    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;

    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;

    return z ^ (z >> 31);

}

static inline f32 rf(u64& st) { return (f32)(sm64(st) & 0xFFFFFF) / (f32)0xFFFFFF; }

static inline f32 rrange(u64& st, f32 lo, f32 hi) { return lo + rf(st) * (hi - lo); }



// ---- Faction colour (civ banner, or pirate red) -------------------------------------------

static bs_color faction_tint(const game_state* s, i16 faction) {

    if (faction >= 0 && faction < s->galaxy.civ_count) return s->galaxy.civs[faction].color;

    return bs_color{ 0.95f, 0.35f, 0.30f, 1.0f };

}



// =====================================================================================

void ai_ships_init(game_state* s) {

    s->npc_ship_count   = 0;

    s->npc_spawned_node = -1;

    s->npc_pop_timer    = 0.0f;

    s->npc_template_ready = FALSE;

    for (i32 i = 0; i < NPC_SHIP_MAX; ++i) s->npc_ships[i].active = FALSE;



    // Shared hull template: loaded once, struct-copied on each spawn (visual texture handles are

    // shared -- safe for rendering, never freed per-agent). Agents fire via direct projectile spawn,

    // so the template needs no Weapon objects.

    if (ship_load(&s->npc_template, "assets/enemy_ship.ship")) {

        ship_visual_resolve_textures(&s->npc_template.visual);

        s->npc_template.faction = VESSEL_PIRATE;

        s->npc_template.faction_id = FACTION_PIRATE;

        s->npc_template.weapon_count = 0;

        s->npc_template.active_weapon_idx = -1;

        for (i32 w = 0; w < SHIP_MAX_WEAPONS; ++w) s->npc_template.weapons[w] = nullptr;

        s->npc_template.glow = s->render.glow_params;

        s->npc_template.radiation_emission = 0.05f;

        s->npc_template_ready = TRUE;

    } else {

        BS_LOG_ERROR("ai_ships_init: failed to load NPC hull template.");

    }

}



// Spawn one agent at an explicit world position, tagged to `faction`, with the given archetype. Its

// home (loiter / return anchor) is the spawn point, so the population is LOCAL to wherever the player

// currently is within the system (dispersion follows the player; distant ships are culled). Returns

// the pool slot, or -1 if the pool is full.

static i32 spawn_npc(game_state* s, i16 faction, HierPos2 pos, i32 home_node, u8 archetype, u64 seed) {

    if (!s->npc_template_ready) return -1;

    i32 slot = -1;

    for (i32 i = 0; i < NPC_SHIP_MAX; ++i) if (!s->npc_ships[i].active) { slot = i; break; }

    if (slot < 0) return -1;

    const AiProfile& p = ai_profile(archetype);

    NpcShip& n = s->npc_ships[slot];

    n.ship = s->npc_template;                 // struct copy (shares visual texture handles)

    n.ship.faction = VESSEL_PIRATE;           // binary friendly-fire enum (stance uses faction_id)

    n.ship.faction_id = faction;

    u64 st = seed ^ (0x9E3779B97F4A7C15ull * (u64)(slot + 1));

    n.rng = st;

    n.ship.origin = pos;

    n.ship.angle  = rrange(st, 0.0f, 2.0f * BS_PI);

    n.flight.velocity = Vec2{ 0.0f, 0.0f };

    n.flight.angular_velocity = 0.0f;

    n.faction   = faction;

    n.archetype = archetype;

    n.state     = AI_PATROL;

    n.hp = n.max_hp = (archetype == ARCHETYPE_MINER) ? 25.0f : (archetype == ARCHETYPE_TRADER ? 30.0f : 40.0f);

    n.home      = pos;                        // loiter / return anchor = local spawn point

    n.home_node = home_node;

    n.orbit_radius = p.patrol_radius * rrange(st, 0.6f, 1.4f);

    n.patrol_wp = pos;

    n.target_ce = -1;

    n.last_seen = pos;

    n.timer       = rrange(st, 0.0f, 2.0f * BS_PI);

    n.state_timer = 0.0f;

    n.fire_cd     = rrange(st, 0.0f, p.fire_period);

    n.work_asteroid    = -1;

    n.work_timer  = 0.0f;

    // Discovery system: key each agent by its (home_node, spawn_seed). Already-discovered agents

    // re-materialize identified so a re-visited system keeps its scanned ships.

    n.spawn_seed  = seed;

    n.discovered  = discovery_npc_is_known(s, home_node, seed);

    n.mission_id  = -1;                        // ambient/garrison by default; macro handoff sets it

    n.active = TRUE;

    if (slot + 1 > s->npc_ship_count) s->npc_ship_count = slot + 1;

    return slot;

}



// A deterministic placement anchor for `role` #index within `node`, derived from the star + planet

// orbit radii. Patrols guard the star/planet orbits; miners work the outer asteroid belt; traders sit

// on planet/station orbits. Positions belong to the SYSTEM (not the player), so ships exist where they

// should when the player arrives -- they are not conjured in a ring around the arrival point.

static HierPos2 system_anchor(game_state* s, i32 node, u8 role, i32 index, u64 seed) {

    const GalaxyNode& gn = s->galaxy.nodes[node];

    HierPos2 star = gn.galaxy_center;

    i32 oc = gn.orbit_count;

    u64 st = seed ^ (0x9E3779B97F4A7C15ull * (u64)(index + 1));

    f32 ang = rrange(st, 0.0f, 2.0f * BS_PI);

    f32 radius;

    if (role == ARCHETYPE_MINER) {

        f32 belt = (oc > 0) ? gn.orbit_radii[oc - 1] : 40000.0f;   // outer belt

        radius = belt * rrange(st, 0.90f, 1.15f);

    } else if (role == ARCHETYPE_TRADER) {

        f32 r = (oc > 0) ? gn.orbit_radii[index % oc] : 25000.0f;  // a planet lane

        radius = r * rrange(st, 0.95f, 1.05f);

    } else { // PATROL / combatants: half at planet orbits, half on a close star ring

        if (oc > 0 && (index % 2 == 0)) radius = gn.orbit_radii[index % oc] * rrange(st, 0.90f, 1.10f);

        else                            radius = ((oc > 0) ? gn.orbit_radii[0] * 0.5f : 20000.0f) * rrange(st, 0.7f, 1.3f);

    }

    return hierpos_add_vec2(&star, vec2_rotate(Vec2{ radius, 0.0f }, ang));

}



// Materialize the FULL population of an owned system at its anchors (patrols at star/planets, miners at

// the belt, traders on planet lanes). Deterministic per node; all civ-tagged. Debug: everything is spawned

// on entry so the whole dispersion is visible immediately.

static void materialize_system(game_state* s, i32 node, i16 owner) {

    u64 seed = (u64)(node + 1) * 0x9E3779B97F4A7C15ull ^ 0xC1FF1EE5ull;

    i32 oc = s->galaxy.nodes[node].orbit_count;

    f32 power = s->galaxy.civs[owner].power;



    i32 patrols = galaxy_history_garrison_at(s, node);   if (patrols > 12) patrols = 12; if (patrols < 2) patrols = 2;

    i32 miners  = 6 + oc * 2 + (i32)(seed % 4ull);       if (miners  > 16) miners  = 16;

    i32 traders = 2 + (i32)(power * 0.3f) + (i32)((seed >> 8) % 3ull); if (traders > 8) traders = 8;



    i32 idx = 0;

    for (i32 k = 0; k < patrols; ++k, ++idx)

        spawn_npc(s, owner, system_anchor(s, node, ARCHETYPE_PATROL, idx, seed), node, ARCHETYPE_PATROL, seed + (u64)idx);

    for (i32 k = 0; k < miners; ++k, ++idx)

        spawn_npc(s, owner, system_anchor(s, node, ARCHETYPE_MINER, idx, seed), node, ARCHETYPE_MINER, seed + (u64)idx);

    for (i32 k = 0; k < traders; ++k, ++idx)

        spawn_npc(s, owner, system_anchor(s, node, ARCHETYPE_TRADER, idx, seed), node, ARCHETYPE_TRADER, seed + (u64)idx);

}



// ---- Population manager: materialize the CURRENT owned system's full population at its anchors ----------

// Per-system (not player-relative): ships are placed at the system's real locations and only re-materialized

// when the player crosses into a DIFFERENT system. So an FTL jump lands you among the destination's existing

// fleet -- nothing spawns in a ring around you.

static void ai_ships_populate(game_state* s) {

    HierPos2 flag = s->fleet_state.fleet.flagship().ship.origin;

    i32 node = galaxy_nearest_node(s, &flag);

    if (node == s->npc_spawned_node) return;   // same system -> keep the existing population



    for (i32 i = 0; i < NPC_SHIP_MAX; ++i) s->npc_ships[i].active = FALSE;   // leaving the old system

    s->npc_ship_count = 0;

    s->npc_spawned_node = node;



    i32 owner = galaxy_history_owner_at_node(s, node);

    if (owner < 0 || owner >= s->galaxy.civ_count || s->galaxy.civs[owner].status != 0) return; // wild: no civ pop

    materialize_system(s, node, (i16)owner);

}



// ---- Macro <-> local handoff (Step 4, station-contract form) --------------------------------------

// Contracts issued by mission-hub stations bind to the CLOSEST unassigned ambient trader in the

// player's current system while they dock at their origin/destination station. Nothing is ever

// spawned here: the ambient population is the only pool contracts draw from -- if no trader is

// free the contract simply waits. Bound traders run the docking loop (ai_trader_tick) and are

// released back to ambient duty when the contract departs, completes, or the link goes stale.



// Station anchor of the mission's CURRENT dock stage (origin while loading, destination while

// unloading). Positions/radii are cached on the mission at contract issue.

static void mission_dock_anchor(const ShipMission& m, HierPos2* out_pos, f32* out_radius) {

    if (m.stage == MISSION_STAGE_MARKET_DOCK) { *out_pos = m.dest_station_pos; *out_radius = m.dest_station_radius; }

    else                                      { *out_pos = m.station_pos;      *out_radius = m.station_radius; }

}



// TRUE while the mission is in a dock stage whose station lies in galaxy node `node`.

static b8 mission_docking_at(const ShipMission& m, i32 node) {

    if (!m.active || m.at_node != node) return FALSE;

    return (m.stage == MISSION_STAGE_ORIGIN_DOCK && m.home_node == node) ||

           (m.stage == MISSION_STAGE_MARKET_DOCK && m.dest_node == node);

}



static void ai_ships_sync_missions(game_state* s) {

    GalaxyState& g = s->galaxy;

    // The galaxy node the player currently occupies (current_system is a cache SLOT, not a node).

    i32 node_here = (g.current_system >= 0 && g.current_system < g.system_count)

                        ? g.cache_node[g.current_system] : -1;



    // Stale-link hygiene: release any trader whose contract is gone, departed, or disagrees on the

    // binding (system change cleared the pool, contract retired, etc.). The ship stays alive and

    // simply returns to ambient duty.

    for (i32 i = 0; i < NPC_SHIP_MAX; ++i) {

        NpcShip& n = s->npc_ships[i];

        if (!n.active || n.mission_id < 0) continue;

        b8 stale = (node_here < 0) || !g.missions ||

                   n.mission_id >= g.mission_count ||

                   !mission_docking_at(g.missions[n.mission_id], node_here) ||

                   g.missions[n.mission_id].ship_slot != i;

        if (stale) {

            n.mission_id = -1;

            if (n.state == AI_TRADE_DOCK || n.state == AI_TRADE_DOCKED) { n.state = AI_PATROL; n.state_timer = 0.0f; }

        }

    }

    if (!g.missions) return;

    // Mirror pass: clear mission->ship links whose ship no longer agrees.

    for (i32 mi = 0; mi < g.mission_count; ++mi) {

        ShipMission& m = g.missions[mi];

        if (m.ship_slot < 0) continue;

        b8 ok = m.active && m.ship_slot < NPC_SHIP_MAX &&

                s->npc_ships[m.ship_slot].active &&

                s->npc_ships[m.ship_slot].mission_id == mi;

        if (!ok) m.ship_slot = -1;

    }

    if (node_here < 0) return;



    // Bind every unbound docking contract in this system to the CLOSEST free ambient trader.

    for (i32 mi = 0; mi < g.mission_count; ++mi) {

        ShipMission& m = g.missions[mi];

        if (m.archetype != ARCHETYPE_TRADER || m.ship_slot >= 0) continue;

        if (!mission_docking_at(m, node_here)) continue;

        HierPos2 st_pos; f32 st_r;

        mission_dock_anchor(m, &st_pos, &st_r);

        i32 best = -1; f32 best_d2 = 3.4e38f;

        for (i32 i = 0; i < NPC_SHIP_MAX; ++i) {

            NpcShip& n = s->npc_ships[i];

            if (!n.active || n.archetype != ARCHETYPE_TRADER || n.mission_id >= 0) continue;

            if (n.home_node != node_here) continue;

            Vec2 d = hierpos_diff(&st_pos, &n.ship.origin, BS_HIERPOS_CELL_SIZE);

            f32 d2 = d.x * d.x + d.y * d.y;

            if (d2 < best_d2) { best_d2 = d2; best = i; }

        }

        if (best < 0) continue;   // no free trader: the contract waits (no spawn fallback)

        s->npc_ships[best].mission_id = mi;

        s->npc_ships[best].work_timer = 0.0f;

        s->npc_ships[best].state      = AI_TRADE_DOCK;

        s->npc_ships[best].state_timer = 0.0f;

        m.ship_slot = best;

        BS_LOG_INFO("ShipAI trade: contract %d picked up by trader %d (station %d)", mi, best, m.station_id);

    }

}



// ---- Perception: acquire the nearest hostile player-fleet ship within sensor range --------------

// An agent only treats the player as a threat when its civilization reads the player as hostile

// (reputation / transitive stance). Inter-civ NPC-vs-NPC targeting is Phase C (needs projectile

// faction ids). Sets target_ce (-1 if none) and remembers last_seen.

static void ai_sense(game_state* s, NpcShip& n) {

    const AiProfile& p = ai_profile(n.archetype);

    n.target_ce = -1;

    if (!galaxy_history_faction_is_hostile(s, n.faction)) return;

    f32 best_d2 = p.sensor_range * p.sensor_range;

    i32 best = -1;

    for (i32 i = 0; i < s->npc_combat_base; ++i) {

        CombatEntity& ce = s->combat_entities[i];

        if (!ce.active || ce.faction_id != FACTION_PLAYER) continue;

        Vec2 to = hierpos_diff(&ce.position, &n.ship.origin, BS_HIERPOS_CELL_SIZE);

        f32 d2 = to.x * to.x + to.y * to.y;

        if (d2 < best_d2) { best_d2 = d2; best = i; }

    }

    n.target_ce = best;

    if (best >= 0) n.last_seen = s->combat_entities[best].position;

}



// ---- MINER: work an asteroid belt by mining per-system asteroids --------------------------------

// Asteroids now live with each StarSystem (see state/game_state.h). The miner remembers its target

// by (home node index -> materialized cache slot, asteroid index). Asteroids are indestructible:

// mining never removes them; the miner just dwells, then moves to another.



// Materialized cache slot holding galaxy node `node_idx` (-1 if not currently resident).

static i32 miner_system_slot(game_state* s, i32 node_idx) {

    if (node_idx < 0) return -1;

    for (i32 i = 0; i < s->galaxy.system_count; ++i)

        if (s->galaxy.cache_node[i] == node_idx) return i;

    return -1;

}



static b8 miner_target_valid(game_state* s, NpcShip& n) {

    if (n.work_asteroid < 0) return FALSE;

    i32 slot = miner_system_slot(s, (i32)n.work_node);   // work_node stores the home node index

    if (slot < 0) return FALSE;

    StarSystem& ss = s->galaxy.systems[slot];

    if (n.work_asteroid >= ss.asteroid_count) return FALSE;

    n.work_pos = ss.asteroids[n.work_asteroid].pos;         // refresh (position is static, but keep in sync)

    return TRUE;

}



static void miner_acquire(game_state* s, NpcShip& n) {

    n.work_asteroid = -1;

    i32 slot = miner_system_slot(s, n.home_node);

    if (slot < 0) return;

    StarSystem& ss = s->galaxy.systems[slot];

    const AiProfile& p = ai_profile(n.archetype);

    f32 r2 = p.sensor_range * p.sensor_range;

    i32 seen = 0;

    for (i32 ai = 0; ai < ss.asteroid_count; ++ai) {

        Vec2 d = hierpos_diff(&ss.asteroids[ai].pos, &n.ship.origin, BS_HIERPOS_CELL_SIZE);

        if (d.x * d.x + d.y * d.y > r2) continue;

        ++seen;

        if ((sm64(n.rng) % (u64)seen) == 0) {          // reservoir-sample one asteroid in range

            n.work_node = n.home_node; n.work_asteroid = ai; n.work_pos = ss.asteroids[ai].pos;

        }

    }

}



// ---- TRADER (contract-bound): dock at the contract's station, dwell, hand back to the macro ----

// Only traders bound to a mission run this loop; unbound ambient traders keep the generic FSM.

// The trader flies to the current dock-stage station, halts facing it, dwells for TRADE_DOCK_TIME,

// then flags the mission's local_ready so the macro stage machine advances (depart / retire).

static void ai_trader_tick(game_state* s, NpcShip& n, f32 dt) {

    GalaxyState& g = s->galaxy;

    if (!g.missions || n.mission_id < 0 || n.mission_id >= g.mission_count) { n.mission_id = -1; return; }

    ShipMission& m = g.missions[n.mission_id];

    const AiProfile& p = ai_profile(n.archetype);

    Ship* sh = &n.ship; ShipFlight* fl = &n.flight;

    n.state_timer += dt;



    HierPos2 st_pos; f32 st_r;

    mission_dock_anchor(m, &st_pos, &st_r);

    Vec2 to = hierpos_diff(&st_pos, &sh->origin, BS_HIERPOS_CELL_SIZE);

    f32 dist = vec2_length(to);

    f32 dock_range = st_r * 1.5f;

    if (dock_range < 600.0f) dock_range = 600.0f;

    const f32 TRADE_DOCK_TIME = 8.0f;   // seconds halted at the station before the stage completes



    if (dist > dock_range) {

        if (n.state != AI_TRADE_DOCK) { n.state = AI_TRADE_DOCK; n.state_timer = 0.0f; }

        n.work_timer = 0.0f;

        steering::apply(sh, fl, steering::arrive(to, p.max_speed, dock_range * 2.0f),

                        p.accel, p.max_speed, p.turn_rate, dt);

    } else {

        if (n.state != AI_TRADE_DOCKED) { n.state = AI_TRADE_DOCKED; n.state_timer = 0.0f; }

        steering::apply_face(sh, fl, Vec2{ 0.0f, 0.0f }, to, p.accel, p.max_speed, p.turn_rate, dt);

        n.work_timer += dt;

        if (n.work_timer >= TRADE_DOCK_TIME && !m.local_ready) {

            m.local_ready = TRUE;          // macro stage machine departs / retires next tick

            n.work_timer = 0.0f;

        }

    }

}



static void ai_miner_tick(game_state* s, NpcShip& n, f32 dt) {

    const AiProfile& p = ai_profile(n.archetype);

    Ship* sh = &n.ship; ShipFlight* fl = &n.flight;

    n.state_timer += dt;



    if (!miner_target_valid(s, n)) miner_acquire(s, n);

    if (!miner_target_valid(s, n)) {

        // No asteroid in reach: loiter around the local home while looking for one.

        f32 orad = (n.orbit_radius > 1.0f) ? n.orbit_radius : p.patrol_radius;

        n.timer += (p.patrol_speed / orad) * dt;

        HierPos2 wp = hierpos_add_vec2(&n.home, vec2_rotate(Vec2{ orad, 0.0f }, n.timer));

        Vec2 to = hierpos_diff(&wp, &sh->origin, BS_HIERPOS_CELL_SIZE);

        steering::apply(sh, fl, steering::arrive(to, p.patrol_speed, 2000.0f),

                        p.accel, p.max_speed, p.turn_rate, dt);

        return;

    }

    // Stop distance scales to the asteroid's radius so the miner halts at the surface.

    f32 ast_r = 800.0f;

    {

        i32 slot = miner_system_slot(s, (i32)n.work_node);

        if (slot >= 0 && n.work_asteroid < s->galaxy.systems[slot].asteroid_count)

            ast_r = s->galaxy.systems[slot].asteroids[n.work_asteroid].radius;

    }

    // Fly to the asteroid; dwell + mine when close, then move on (asteroids are NOT destroyed).

    Vec2 to = hierpos_diff(&n.work_pos, &sh->origin, BS_HIERPOS_CELL_SIZE);

    f32 dist = vec2_length(to);

    f32 MINE_RANGE = ast_r * 1.4f;

    if (MINE_RANGE < 600.0f) MINE_RANGE = 600.0f;

    const f32 MINE_TIME = 6.0f;

    if (dist > MINE_RANGE) {

        n.work_timer = 0.0f;

        steering::apply(sh, fl, steering::arrive(to, p.patrol_speed, 3000.0f),

                        p.accel, p.max_speed, p.turn_rate, dt);

    } else {

        steering::apply_face(sh, fl, Vec2{ 0.0f, 0.0f }, to, p.accel, p.max_speed, p.turn_rate, dt);

        n.work_timer += dt;

        if (n.work_timer >= MINE_TIME) {

            n.work_asteroid = -1; n.work_timer = 0.0f;      // done dwelling -> pick another asteroid next tick

        }

    }

}



// ---- Per-agent behavior: FSM (PATROL -> PURSUE -> ATTACK -> RETURN) driven by perception ---------

static void ai_ship_tick(game_state* s, NpcShip& n, f32 dt) {

    const AiProfile& p = ai_profile(n.archetype);

    Ship* sh = &n.ship;

    ShipFlight* fl = &n.flight;



    // Civilian miners run their own work loop (no combat pursuit).

    if (n.archetype == ARCHETYPE_MINER) { ai_miner_tick(s, n, dt); return; }



    // Contract-bound traders run the station docking loop; unbound traders stay generic.

    if (n.archetype == ARCHETYPE_TRADER && n.mission_id >= 0) { ai_trader_tick(s, n, dt); return; }



    n.state_timer += dt;

    if (n.fire_cd > 0.0f) n.fire_cd -= dt;



    ai_sense(s, n);



    Vec2 to_home = hierpos_diff(&n.home, &sh->origin, BS_HIERPOS_CELL_SIZE);

    f32  home_dist = vec2_length(to_home);



    // Resolve the current target (a live player-fleet combat entity).

    b8 have_target = (n.target_ce >= 0 && n.target_ce < s->npc_combat_base &&

                      s->combat_entities[n.target_ce].active &&

                      s->combat_entities[n.target_ce].faction_id == FACTION_PLAYER);

    Vec2 to_target{ 0.0f, 0.0f };

    f32  tdist = 0.0f;

    if (have_target) {

        to_target = hierpos_diff(&s->combat_entities[n.target_ce].position, &sh->origin, BS_HIERPOS_CELL_SIZE);

        tdist = vec2_length(to_target);

    }



    // ---- State transitions ----

    u8 st = n.state;

    if (home_dist > p.leash_range) {

        st = AI_RETURN;                                  // leashed: break off and go home

    } else {

        switch (st) {

            case AI_PATROL:  if (have_target && p.aggression >= 0.5f) st = AI_PURSUE; break;

            case AI_PURSUE:  if (!have_target) st = AI_PATROL;

                             else if (tdist <= p.engage_range) st = AI_ATTACK; break;

            case AI_ATTACK:  if (!have_target) st = AI_PATROL;

                             else if (tdist > p.engage_range * 1.2f) st = AI_PURSUE; break;

            case AI_RETURN:  if (home_dist <= p.patrol_radius) st = AI_PATROL; break;

            default:         st = AI_PATROL; break;

        }

    }

    if (st != n.state) { n.state = st; n.state_timer = 0.0f; }



    // ---- Act ----

    switch (n.state) {

        case AI_PATROL: {

            f32 orad  = (n.orbit_radius > 1.0f) ? n.orbit_radius : p.patrol_radius;

            f32 omega = p.patrol_speed / orad;

            n.timer += omega * dt;

            Vec2 orbit = vec2_rotate(Vec2{ orad, 0.0f }, n.timer);

            n.patrol_wp = hierpos_add_vec2(&n.home, orbit);

            Vec2 to_wp = hierpos_diff(&n.patrol_wp, &sh->origin, BS_HIERPOS_CELL_SIZE);

            steering::apply(sh, fl, steering::arrive(to_wp, p.patrol_speed, 3000.0f),

                            p.accel, p.max_speed, p.turn_rate, dt);

        } break;

        case AI_PURSUE: {

            steering::apply(sh, fl, steering::seek(to_target, p.max_speed),

                            p.accel, p.max_speed, p.turn_rate, dt);

        } break;

        case AI_ATTACK: {

            steering::apply_face(sh, fl, steering::standoff(to_target, tdist, p.standoff, p.max_speed),

                                 to_target, p.accel, p.max_speed, p.turn_rate, dt);

            // Fire when roughly aligned and in the firing band.

            if (tdist >= p.min_range && tdist <= p.engage_range && n.fire_cd <= 0.0f) {

                f32 desired_angle = atan2f(-to_target.x, to_target.y);

                f32 ad = desired_angle - sh->angle;

                while (ad >  BS_PI) ad -= 2.0f * BS_PI;

                while (ad < -BS_PI) ad += 2.0f * BS_PI;

                if (fabsf(ad) < 0.30f) {

                    Vec2 v = vec2_scale(to_target, 12000.0f / (tdist > 1.0f ? tdist : 1.0f));

                    s->projectiles.spawn(sh->origin, v, 8.0f, 4.0f,

                                         bs_color{ 1.0f, 0.5f, 0.3f, 1.0f }, VESSEL_PIRATE, 0.4f, 1.0f);

                    n.fire_cd = p.fire_period;

                }

            }

        } break;

        case AI_RETURN: {

            f32 slow = (n.orbit_radius > 1.0f) ? n.orbit_radius : p.patrol_radius;

            steering::apply(sh, fl, steering::arrive(to_home, p.max_speed, slow),

                            p.accel, p.max_speed, p.turn_rate, dt);

        } break;

        default: break;

    }

}



void ai_ships_update(game_state* s, f32 dt) {

    if (!s) return;

    // Population manager (throttled): keep the current owned system garrisoned.

    s->npc_pop_timer -= dt;

    if (s->npc_pop_timer <= 0.0f) {

        s->npc_pop_timer = 0.3f;

        ai_ships_populate(s);

    }

    // Macro handoff: keep co-located trade missions in sync with live agents. Runs every frame

    // (after populate) so mission ships survive the on-system-change pool clear and track arrivals.

    ai_ships_sync_missions(s);

    for (i32 i = 0; i < NPC_SHIP_MAX; ++i) {

        if (s->npc_ships[i].active) ai_ship_tick(s, s->npc_ships[i], dt);

    }

}



// Register every active agent into the NPC window of combat_entities[] so projectiles hit them and

// the RTS layer can hover / order-attack them exactly like the player fleet + enemy hull.

void ai_ships_register_combat(game_state* s) {

    if (!s) return;

    s->combat_entity_count = s->npc_combat_base;

    for (i32 i = 0; i < NPC_SHIP_MAX; ++i) {

        NpcShip& n = s->npc_ships[i];

        if (!n.active) continue;

        if (s->combat_entity_count >= MAX_COMBAT_ENTITIES) break;

        CombatEntity* ce = &s->combat_entities[s->combat_entity_count++];

        *ce = CombatEntity{};

        ce->active     = TRUE;

        ce->position   = n.ship.origin;

        ce->velocity   = n.flight.velocity;

        ce->radius     = ship_bounding_radius(&n.ship);

        ce->faction    = VESSEL_PIRATE;

        ce->faction_id = n.faction;

        ce->is_npc     = TRUE;

        ce->npc_index  = i;

        ce->hp         = n.hp;

        ce->ship       = &n.ship;

        ce->tint       = faction_tint(s, n.faction);

        ce->radiation_emission = n.ship.radiation_emission;

        ce->is_drone   = FALSE;

    }

}



void ai_ship_damage(game_state* s, i32 npc_index, f32 dmg) {

    if (!s || npc_index < 0 || npc_index >= NPC_SHIP_MAX) return;

    NpcShip& n = s->npc_ships[npc_index];

    if (!n.active) return;

    n.hp -= dmg;

    if (n.hp <= 0.0f) {

        n.active = FALSE;

        // Destroying a patrol is an act of aggression against its civilization.

        if (n.faction >= 0) galaxy_history_player_raid(s, n.faction, 3.0f);

        if (n.mission_id >= 0) {

            // A cross-system traveler (not a garrison ship): retire the macro mission instead of

            // decrementing the local garrison, so the destroyed trader stops travelling/rendering.

            ship_mission_notify_destroyed(s, n.mission_id);

        } else {

            // Step B: the loss persists in the macro fleet layer (garrison shrinks; re-materialises fewer).

            galaxy_history_garrison_add(s, n.home_node, -1);

        }

    }

}

