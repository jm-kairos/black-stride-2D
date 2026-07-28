#include "sim/ai_ship.h"

#include "game.h"                 // full game_state (Ship, ShipFlight, projectiles, galaxy)

#include "sim/galaxy_map.h"       // galaxy_nearest_node

#include "sim/galaxy_history.h"   // owner_at_node / faction_is_hostile / player_raid

#include "sim/ship_mission.h"     // ship_mission_notify_destroyed (macro <-> local handoff)

#include "render/ship_visual.h"          // ship_visual_resolve_textures

#include "sim/steering.h"         // shared steering (arrive/seek/standoff + apply)

#include "sim/station_market.h"   // market writeback (miners deliver, ambient traders micro-haul)

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

    n.work_station = -1;

    n.cargo_units  = 0.0f;

    n.cargo_good   = 0;

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

        f32 belt = (oc > 0) ? gn.orbit_radii[oc - 1] : 80000.0f;   // outer belt

        radius = belt * rrange(st, 0.90f, 1.15f);

    } else if (role == ARCHETYPE_TRADER) {

        f32 r = (oc > 0) ? gn.orbit_radii[index % oc] : 50000.0f;  // a planet lane

        radius = r * rrange(st, 0.95f, 1.05f);

    } else { // PATROL / combatants: half at planet orbits, half on a close star ring

        if (oc > 0 && (index % 2 == 0)) radius = gn.orbit_radii[index % oc] * rrange(st, 0.90f, 1.10f);

        else                            radius = ((oc > 0) ? gn.orbit_radii[0] * 0.5f : 40000.0f) * rrange(st, 0.7f, 1.3f);

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



// Wild / unclaimed space is not empty: a deterministic handful of pirate raiders (FACTION_PIRATE)
// camp the star ring of a lawless system and engage anyone on sight (pairwise stance: pirates are
// hostile to all factions). Roughly 1 in 6 wild systems rolls empty so lawless space stays uneven.
static void materialize_wild_system(game_state* s, i32 node) {

    u64 st = (u64)(node + 1) * 0x9E3779B97F4A7C15ull ^ 0xBADC0FFEEull;

    i32 pirates = (i32)(sm64(st) % 6ull);   // 0..5 raiders

    for (i32 k = 0; k < pirates; ++k)

        spawn_npc(s, FACTION_PIRATE, system_anchor(s, node, ARCHETYPE_PIRATE, k, st), node, ARCHETYPE_PIRATE, st + (u64)(k + 1));

    if (pirates > 0)

        BS_LOG_INFO("ShipAI wild: node %d materialized %d pirate raider(s)", node, pirates);

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

    if (owner < 0 || owner >= s->galaxy.civ_count || s->galaxy.civs[owner].status != 0) {

        materialize_wild_system(s, node);   // lawless space: pirates instead of a civ population

        return;

    }

    materialize_system(s, node, (i16)owner);

}



// ---- Macro <-> local handoff (Phase 2: full traveler materialisation) -----------------------------

// Contracts issued by mission-hub stations get a live NpcShip form whenever their in-system portion
// happens in the player's current system. ORIGIN_DOCK binds the CLOSEST unassigned ambient trader
// (the contract is "picked up" locally; if none is free the contract waits). Every other in-system
// stage (acquire / depart / cross / final approach / market dock) SPAWNS the traveler itself at the
// macro position. While bound (ship_slot >= 0) the live agent owns motion: it flies the macro's
// current leg (AI_TRAVEL_LEG) or runs the docking loop, mirrors its position into ShipMission::pos,
// and hands stage completion back through local_ready. Native traders released at contract end
// return to ambient duty; travelers that jump out or belong elsewhere despawn with the ship.



// Station anchor of the mission's CURRENT dock stage (origin while loading, destination while

// unloading). Positions/radii are cached on the mission at contract issue.

static void mission_dock_anchor(const ShipMission& m, HierPos2* out_pos, f32* out_radius) {

    if (m.stage == MISSION_STAGE_MARKET_DOCK) { *out_pos = m.dest_station_pos; *out_radius = m.dest_station_radius; }

    else                                      { *out_pos = m.station_pos;      *out_radius = m.station_radius; }

}



// Phase 2: TRUE while mission `m` should have a live agent in galaxy node `node`: docking at a
// station here, or flying an in-system leg here (acquire / depart / cross / final approach).
// JUMP (interstellar) and COOLDOWN never materialize.
static b8 mission_live_here(const ShipMission& m, i32 node) {

    if (!m.active || node < 0 || m.at_node != node) return FALSE;

    switch (m.stage) {

        case MISSION_STAGE_ORIGIN_DOCK:    return m.home_node == node;

        case MISSION_STAGE_MARKET_DOCK:    return m.dest_node == node;

        case MISSION_STAGE_ACQUIRE:

        case MISSION_STAGE_TO_JUMP:

        case MISSION_STAGE_CROSS:

        case MISSION_STAGE_FINAL_APPROACH: return TRUE;

        default:                           return FALSE;

    }

}



static void ai_ships_sync_missions(game_state* s) {

    GalaxyState& g = s->galaxy;

    // The galaxy node the player currently occupies (current_system is a cache SLOT, not a node).

    i32 node_here = (g.current_system >= 0 && g.current_system < g.system_count)

                        ? g.cache_node[g.current_system] : -1;



    // Stale-link hygiene (Phase 2): a linked agent whose contract is gone, jumped out, or disagrees
    // on the binding is resolved here. NATIVE traders (home in this system) return to ambient duty;
    // travelers that jumped out or belong to another system despawn — they left with the ship.

    for (i32 i = 0; i < NPC_SHIP_MAX; ++i) {

        NpcShip& n = s->npc_ships[i];

        if (!n.active || n.mission_id < 0) continue;

        const ShipMission* m = (g.missions && n.mission_id < g.mission_count) ? &g.missions[n.mission_id] : nullptr;

        b8 stale = !m || !mission_live_here(*m, node_here) || m->ship_slot != i;

        if (!stale) continue;

        b8 jumped_out = m && m->active && m->stage == MISSION_STAGE_JUMP;

        b8 native     = (node_here >= 0) && (n.home_node == node_here);

        n.mission_id = -1;

        if (jumped_out || !native) {

            n.active = FALSE;   // the traveler departed (or is foreign): the hull leaves with it

            if (jumped_out) BS_LOG_INFO("ShipAI travel: mission %d jumped out (live agent %d released)", (i32)(m - g.missions), i);

        } else if (n.state == AI_TRADE_DOCK || n.state == AI_TRADE_DOCKED || n.state == AI_TRAVEL_LEG) {

            n.state = AI_PATROL; n.state_timer = 0.0f;   // native trader returns to ambient duty

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



    // Bind or spawn the live form of every unbound contract present in this system (Phase 2):
    // ORIGIN_DOCK binds the CLOSEST free ambient trader (the contract is "picked up" locally);
    // every other live stage (in-system legs, market dock) spawns the traveler itself at the
    // macro position — a visiting ship the player can meet, follow, or destroy.

    for (i32 mi = 0; mi < g.mission_count; ++mi) {

        ShipMission& m = g.missions[mi];

        if ((m.archetype != ARCHETYPE_TRADER && m.archetype != ARCHETYPE_WARSHIP) || m.ship_slot >= 0) continue;

        if (!mission_live_here(m, node_here)) continue;

        if (m.stage != MISSION_STAGE_ORIGIN_DOCK) {

            // In-transit / visiting trader: materialize it exactly where the macro says it is.

            HierPos2 pos = ship_mission_position(s, m);

            i16 owner = (m.owner >= 0) ? m.owner : FACTION_PIRATE;

            i32 slot = spawn_npc(s, owner, pos, m.home_node, m.archetype, m.seed);

            if (slot < 0) continue;   // pool full: the macro keeps integrating this leg itself

            NpcShip& t = s->npc_ships[slot];

            t.mission_id  = mi;

            t.state       = (m.stage == MISSION_STAGE_MARKET_DOCK) ? AI_TRADE_DOCK : AI_TRAVEL_LEG;

            t.state_timer = 0.0f;

            m.ship_slot   = slot;

            BS_LOG_INFO("ShipAI travel: mission %d materialized in-system (stage %u, node %d, agent %d)",
                        mi, (u32)m.stage, node_here, slot);

            continue;

        }

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



// ---- Perception: acquire the nearest hostile combat entity within sensor range ------------------
// Phase 1 (autonomous universe): agents sense EVERY combat entity (player fleet, patrol hull, other
// NPCs) and resolve hostility pairwise via galaxy_history_factions_hostile — so civ patrols fight
// warring civs and pirates, not just the player. `self` is this agent's npc_ships[] index, used to
// skip its own (one-frame-stale) registration in combat_entities[]. Sets target_ce (-1 if none)
// and remembers last_seen.
static void ai_sense(game_state* s, NpcShip& n, i32 self) {
    const AiProfile& p = ai_profile(n.archetype);
    n.target_ce = -1;
    f32 best_d2 = p.sensor_range * p.sensor_range;
    i32 best = -1;
    for (i32 i = 0; i < s->combat_entity_count; ++i) {
        CombatEntity& ce = s->combat_entities[i];
        if (!ce.active) continue;
        if (ce.is_npc && ce.npc_index == self) continue;               // never target yourself
        if (!galaxy_history_factions_hostile(s, n.faction, ce.faction_id)) continue;
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



// Nearest station (index into StarSystem::stations[]) of materialized slot `slot`; -1 if none.

static i32 nearest_station(game_state* s, i32 slot, const HierPos2* from) {

    if (slot < 0) return -1;

    StarSystem& ss = s->galaxy.systems[slot];

    i32 best = -1; f32 best_d2 = 3.4e38f;

    for (i32 i = 0; i < ss.station_count; ++i) {

        Vec2 d = hierpos_diff(&ss.stations[i].pos, from, BS_HIERPOS_CELL_SIZE);

        f32 d2 = d.x * d.x + d.y * d.y;

        if (d2 < best_d2) { best_d2 = d2; best = i; }

    }

    return best;

}



// What a dwell at the rock yields: the node's dominant resource, occasionally its rare form.

static u8 miner_pick_good(game_state* s, NpcShip& n) {

    if (n.home_node < 0 || n.home_node >= s->galaxy.node_count) return GOOD_IRON_ORE;

    const GalaxyNode& gn = s->galaxy.nodes[n.home_node];

    b8 rare = (sm64(n.rng) % 4ull) == 0;   // 1-in-4 hauls strike the rare form

    if (gn.res_metal >= gn.res_volatiles) return rare ? GOOD_RARE_METALS   : GOOD_IRON_ORE;

    return                                       rare ? GOOD_HYDROGEN_FUEL : GOOD_WATER_ICE;

}



// Phase 3: AI_DELIVER — full hold, fly to the chosen station, dock, and SELL the ore into its

// market (station_market_apply: stock up, local price down). Mining now moves the economy.

static void ai_miner_deliver(game_state* s, NpcShip& n, f32 dt) {

    const AiProfile& p = ai_profile(n.archetype);

    Ship* sh = &n.ship; ShipFlight* fl = &n.flight;

    i32 slot = miner_system_slot(s, n.home_node);

    if (slot < 0 || n.work_station < 0 || n.work_station >= s->galaxy.systems[slot].station_count) {

        n.work_station = -1; n.state = AI_PATROL; n.state_timer = 0.0f;   // system swapped out: abort

        return;

    }

    SystemStation& st = s->galaxy.systems[slot].stations[n.work_station];

    Vec2 to = hierpos_diff(&st.pos, &sh->origin, BS_HIERPOS_CELL_SIZE);

    f32 dist = vec2_length(to);

    f32 dock_range = st.radius * 1.5f;

    if (dock_range < 600.0f) dock_range = 600.0f;

    const f32 UNLOAD_TIME = 4.0f;

    if (dist > dock_range) {

        n.work_timer = 0.0f;

        steering::apply(sh, fl, steering::arrive(to, p.max_speed, dock_range * 2.0f),

                        p.accel, p.max_speed, p.turn_rate, dt);

        return;

    }

    steering::apply_face(sh, fl, Vec2{ 0.0f, 0.0f }, to, p.accel, p.max_speed, p.turn_rate, dt);

    n.work_timer += dt;

    if (n.work_timer < UNLOAD_TIME) return;

    station_market_apply(s, st.station_id, n.cargo_good, n.cargo_units);

    BS_LOG_INFO("ShipAI mining: miner %d delivered %.0f %s to station N%d/%d",

                (i32)(&n - s->npc_ships), n.cargo_units, TRADE_GOOD_NAMES[n.cargo_good],

                station_id_node(st.station_id), station_id_index(st.station_id));

    n.cargo_units = 0.0f; n.work_timer = 0.0f; n.work_station = -1;

    n.state = AI_PATROL; n.state_timer = 0.0f;

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



    // Phase 2 movement stages: the live agent OWNS the macro's current in-system leg. It flies at
    // macro speed (schedule coherence with unobserved missions), mirrors its position into m.pos so
    // the galaxy-map pip tracks the real hull, and hands arrival back via local_ready.

    if (m.stage == MISSION_STAGE_ACQUIRE || m.stage == MISSION_STAGE_TO_JUMP ||

        m.stage == MISSION_STAGE_CROSS   || m.stage == MISSION_STAGE_FINAL_APPROACH) {

        if (n.state != AI_TRAVEL_LEG) { n.state = AI_TRAVEL_LEG; n.state_timer = 0.0f; }

        f32 spd = (g.ai_speed_in_system > 0.0f) ? g.ai_speed_in_system : 50000.0f;

        Vec2 leg = hierpos_diff(&m.leg_target, &sh->origin, BS_HIERPOS_CELL_SIZE);

        steering::apply(sh, fl, steering::arrive(leg, spd, spd * 2.0f), spd, spd, p.turn_rate * 2.0f, dt);

        m.pos = sh->origin;   // macro mirrors the live hull

        if (vec2_length(leg) <= 1500.0f && !m.local_ready) m.local_ready = TRUE;

        return;

    }



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

    // Full hold takes priority: run the delivery leg (Phase 3).

    if (n.state == AI_DELIVER) { ai_miner_deliver(s, n, dt); return; }

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

            // Phase 3: the dwell actually yields ore into the hold; a full hold triggers delivery.

            const f32 MINE_YIELD = 8.0f, HOLD_MAX = 24.0f;

            if (n.cargo_units <= 0.0f) n.cargo_good = miner_pick_good(s, n);

            n.cargo_units += MINE_YIELD;

            if (n.cargo_units >= HOLD_MAX) {

                i32 st = nearest_station(s, miner_system_slot(s, n.home_node), &sh->origin);

                if (st >= 0) { n.work_station = st; n.state = AI_DELIVER; n.state_timer = 0.0f; }

                else n.cargo_units = 0.0f;   // no stations in reach (wild/unowned): vent and keep working

            }

        }

    }

}



// ---- TRADER (ambient, Phase 3): intra-system micro-hauls -----------------------------------------

// Unbound traders in a materialized system run a real buy->sell loop: load the biggest-stock good

// at one station (stock down -> price up), sell it at another (stock up -> price down). Systems

// with a single station run a planet<->station shuttle delivering planetary exports instead.

// Returns FALSE when no local market work is possible (caller falls back to the generic FSM).

static u8 planet_export_good(game_state* s, i32 node, u64& rng) {

    if (node < 0 || node >= s->galaxy.node_count) return GOOD_GRAIN;

    const GalaxyNode& gn = s->galaxy.nodes[node];

    if (gn.biosphere >= gn.res_metal && gn.biosphere >= gn.res_volatiles)

        return ((sm64(rng) & 1ull) == 0) ? GOOD_GRAIN : GOOD_ORGANICS;

    return (gn.res_metal >= gn.res_volatiles) ? GOOD_IRON_ORE : GOOD_WATER_ICE;

}



static b8 ai_ambient_trader_tick(game_state* s, NpcShip& n, f32 dt) {

    i32 slot = miner_system_slot(s, n.home_node);

    if (slot < 0) return FALSE;

    StarSystem& ss = s->galaxy.systems[slot];

    if (ss.station_count <= 0) return FALSE;

    const AiProfile& p = ai_profile(n.archetype);

    Ship* sh = &n.ship; ShipFlight* fl = &n.flight;

    n.state_timer += dt;

    const f32 HAUL_UNITS = 10.0f, DOCK_TIME = 5.0f;

    b8 single = ss.station_count < 2;



    // Single-station system, empty hold: shuttle out to a planet lane and load its exports.

    if (single && n.cargo_units <= 0.0f) {

        HierPos2 anchor = system_anchor(s, n.home_node, ARCHETYPE_TRADER, 1, n.spawn_seed);

        Vec2 to = hierpos_diff(&anchor, &sh->origin, BS_HIERPOS_CELL_SIZE);

        if (vec2_length(to) > 1500.0f) {

            if (n.state != AI_TRADE_DOCK) { n.state = AI_TRADE_DOCK; n.state_timer = 0.0f; }

            n.work_timer = 0.0f;

            steering::apply(sh, fl, steering::arrive(to, p.max_speed, 3000.0f),

                            p.accel, p.max_speed, p.turn_rate, dt);

            return TRUE;

        }

        steering::apply_face(sh, fl, Vec2{ 0.0f, 0.0f }, to, p.accel, p.max_speed, p.turn_rate, dt);

        n.work_timer += dt;

        if (n.work_timer >= DOCK_TIME) {

            n.cargo_good  = planet_export_good(s, n.home_node, n.rng);

            n.cargo_units = HAUL_UNITS * 0.6f;

            n.work_station = 0; n.work_timer = 0.0f;

        }

        return TRUE;

    }



    // Choose a station target if none: empty hold -> a buy stop, loaded -> a sell stop.

    if (n.work_station < 0 || n.work_station >= ss.station_count) {

        n.work_station = (i32)(sm64(n.rng) % (u64)ss.station_count);

        n.work_timer   = 0.0f;

    }

    SystemStation& st = ss.stations[n.work_station];

    Vec2 to = hierpos_diff(&st.pos, &sh->origin, BS_HIERPOS_CELL_SIZE);

    f32 dist = vec2_length(to);

    f32 dock_range = st.radius * 1.5f;

    if (dock_range < 600.0f) dock_range = 600.0f;

    if (dist > dock_range) {

        if (n.state != AI_TRADE_DOCK) { n.state = AI_TRADE_DOCK; n.state_timer = 0.0f; }

        n.work_timer = 0.0f;

        steering::apply(sh, fl, steering::arrive(to, p.max_speed, dock_range * 2.0f),

                        p.accel, p.max_speed, p.turn_rate, dt);

        return TRUE;

    }

    if (n.state != AI_TRADE_DOCKED) { n.state = AI_TRADE_DOCKED; n.state_timer = 0.0f; }

    steering::apply_face(sh, fl, Vec2{ 0.0f, 0.0f }, to, p.accel, p.max_speed, p.turn_rate, dt);

    n.work_timer += dt;

    if (n.work_timer < DOCK_TIME) return TRUE;

    n.work_timer = 0.0f;

    if (n.cargo_units > 0.0f) {

        station_market_apply(s, st.station_id, n.cargo_good, n.cargo_units);

        BS_LOG_INFO("ShipAI micro-trade: trader %d sold %.0f %s at station N%d/%d",

                    (i32)(&n - s->npc_ships), n.cargo_units, TRADE_GOOD_NAMES[n.cargo_good],

                    station_id_node(st.station_id), station_id_index(st.station_id));

        n.cargo_units = 0.0f;

        n.work_station = -1;   // next: pick a buy stop (or the planet leg in single-station systems)

        return TRUE;

    }

    // Buy the good this station is most glutted with (respecting available stock).

    MarketGood mg[GOOD_COUNT];

    station_market_get(s, st.station_id, mg);

    i32 best = -1; f32 best_stock = 1.0f;

    for (i32 g = 0; g < GOOD_COUNT; ++g)

        if (mg[g].stock > best_stock) { best_stock = mg[g].stock; best = g; }

    if (best < 0) { n.work_station = -1; return TRUE; }   // bare shelves: try another station

    f32 units = (best_stock < HAUL_UNITS) ? best_stock : HAUL_UNITS;

    station_market_apply(s, st.station_id, best, -units);

    n.cargo_good  = (u8)best;

    n.cargo_units = units;

    i32 nxt = (i32)(sm64(n.rng) % (u64)ss.station_count);

    if (nxt == n.work_station) nxt = (nxt + 1) % ss.station_count;

    n.work_station = nxt;   // sell stop: a different station

    return TRUE;

}



// ---- WARSHIP (mission-bound, Phase 4): fly the column's leg, break off to engage hostiles --------

// Reinforcement columns and patrols in the player's system fly their macro leg exactly like a

// materialized trader, but they are combatants: any hostile contact hands control to the generic

// combat FSM (return FALSE) until the threat is gone, then the column resumes its leg.

static b8 ai_mission_warship_tick(game_state* s, NpcShip& n, i32 self, f32 dt) {

    GalaxyState& g = s->galaxy;

    if (!g.missions || n.mission_id < 0 || n.mission_id >= g.mission_count) { n.mission_id = -1; return FALSE; }

    ShipMission& m = g.missions[n.mission_id];

    ai_sense(s, n, self);

    if (n.target_ce >= 0) return FALSE;   // hostiles in sensor range: fight first

    const AiProfile& p = ai_profile(n.archetype);

    Ship* sh = &n.ship; ShipFlight* fl = &n.flight;

    n.state_timer += dt;

    if (n.state != AI_TRAVEL_LEG) { n.state = AI_TRAVEL_LEG; n.state_timer = 0.0f; }

    f32 spd = (g.ai_speed_in_system > 0.0f) ? g.ai_speed_in_system : 50000.0f;

    Vec2 leg = hierpos_diff(&m.leg_target, &sh->origin, BS_HIERPOS_CELL_SIZE);

    steering::apply(sh, fl, steering::arrive(leg, spd, spd * 2.0f), spd, spd, p.turn_rate * 2.0f, dt);

    m.pos = sh->origin;   // macro mirrors the live hull

    if (vec2_length(leg) <= 1500.0f && !m.local_ready) m.local_ready = TRUE;

    return TRUE;

}



// ---- Per-agent behavior: FSM (PATROL -> PURSUE -> ATTACK -> RETURN) driven by perception ---------
// `self` = this agent's npc_ships[] index (perception self-skip + damage attribution).
static void ai_ship_tick(game_state* s, NpcShip& n, i32 self, f32 dt) {

    const AiProfile& p = ai_profile(n.archetype);

    Ship* sh = &n.ship;

    ShipFlight* fl = &n.flight;



    // Civilian miners run their own work loop (no combat pursuit).

    if (n.archetype == ARCHETYPE_MINER) { ai_miner_tick(s, n, dt); return; }



    // Contract-bound traders run the station docking loop; unbound traders run intra-system

    // micro-hauls when the local market is materialized (Phase 3), else the generic FSM.

    if (n.archetype == ARCHETYPE_TRADER && n.mission_id >= 0) { ai_trader_tick(s, n, dt); return; }

    if (n.archetype == ARCHETYPE_TRADER && ai_ambient_trader_tick(s, n, dt)) return;

    // Mission-bound warships (Phase 4) fly their column's leg unless there is something to fight.

    if (n.archetype == ARCHETYPE_WARSHIP && n.mission_id >= 0 && ai_mission_warship_tick(s, n, self, dt)) return;



    n.state_timer += dt;

    if (n.fire_cd > 0.0f) n.fire_cd -= dt;



    ai_sense(s, n, self);



    Vec2 to_home = hierpos_diff(&n.home, &sh->origin, BS_HIERPOS_CELL_SIZE);

    f32  home_dist = vec2_length(to_home);



    // Resolve the current target (any live, still-hostile combat entity).

    b8 have_target = (n.target_ce >= 0 && n.target_ce < s->combat_entity_count &&

                      s->combat_entities[n.target_ce].active &&

                      galaxy_history_factions_hostile(s, n.faction, s->combat_entities[n.target_ce].faction_id));

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

                                         bs_color{ 1.0f, 0.5f, 0.3f, 1.0f }, VESSEL_PIRATE, n.faction, 0.4f, 1.0f);

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

        if (s->npc_ships[i].active) ai_ship_tick(s, s->npc_ships[i], i, dt);

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



void ai_ship_damage(game_state* s, i32 npc_index, f32 dmg, i16 attacker_faction) {

    if (!s || npc_index < 0 || npc_index >= NPC_SHIP_MAX) return;

    NpcShip& n = s->npc_ships[npc_index];

    if (!n.active) return;

    n.hp -= dmg;

    if (n.hp <= 0.0f) {

        n.active = FALSE;

        BS_LOG_INFO("ShipAI combat: npc %d (archetype %u, faction %d) destroyed by faction %d",
                    npc_index, (u32)n.archetype, (i32)n.faction, (i32)attacker_faction);

        // Reputation only reacts to PLAYER kills: destroying a civ ship is an act of aggression
        // against its civilization. NPC-vs-NPC kills (wars, pirates) carry no player rep change.

        if (n.faction >= 0 && attacker_faction == FACTION_PLAYER)
            galaxy_history_player_raid(s, n.faction, 3.0f);

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

