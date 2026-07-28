#include "sim/ship_mission.h"

#include "game.h"              // full game_state + ShipMission definition

#include "sim/galaxy_map.h"    // galaxy_route_find, galaxy_lane_length

#include "sim/ai_ship.h"       // ShipArchetype (ARCHETYPE_TRADER)

#include "sim/station_market.h" // minimal economy: baseline markets + trader-dock deltas

#include <core/logger.h>       // BS_LOG_INFO

#include <math.h>



using namespace bs_math;

using GalaxyState = game_state::GalaxyState;



// ---- Tuning -------------------------------------------------------------------------------

// Movement speeds live in GalaxyState (ai_speed_in_system / ai_speed_jump, editor-tunable) in

// world units per SECOND of simulation. The game clock maps 1 real second == 1 in-game hour at

// 1x (game_state::sim_dt), so `hours` below double as seconds and everything scales with time

// acceleration automatically.



// Jump-circle radius fallback for a node with no recorded planet orbits (comparable to 2x a

// typical outer orbit, which spans ~3M..16M world units).

static const f32 JUMP_RADIUS_FALLBACK = 3.2e7f;



// Full-route scratch capacity. Longer than the galaxy diameter (~66 hops observed) so a complete

// route always fits here even though a mission only CACHES the first MISSION_ROUTE_MAX hops; when

// a mission exhausts its cached chunk mid-journey it re-paths from its current node for the next.

static const i32 ROUTE_SCRATCH = 256;



// ---- Trade objective tuning (Step 3) ------------------------------------------------------

// In-game hours a trader dwells at each endpoint "delivering" before departing again. At the

// per-frame sim clamp (~0.05 h/frame) this is a ~6s visible pause at 1x, less under time accel.

// Used as the MACRO stand-in for docking when the player is not in the mission's system.

static const f32 TRADE_DWELL_HOURS = 18.0f;



// In-game hours a mission-hub station waits after a contract completes (or its ship is destroyed)

// before issuing the next contract.

static const f32 STATION_CONTRACT_COOLDOWN = 60.0f;



// How many owned nodes to sample when choosing a market; the highest trade-value sample wins,

// biased by a random roll so routes vary between traders and across successive runs.

static const i32 TRADE_MARKET_SAMPLES = 8;



// Four-tier economy weights: closer/same-civ destinations score exponentially higher.
// Tier 0 (intra-system, 0 hops): 1000x — trade within the same system
// Tier 1 (intra-civ, 1-2 hops, same owner): 100x — trade within the civilization
// Tier 2 (nearby uninhabited, 1-2 hops): 10x — frontier trade to close unowned systems
// Tier 3 (inter-civ global, 3+ hops): 1x — rare long-distance cross-civ trade
static const f32 TRADE_TIER0_WEIGHT = 1000.0f;
static const f32 TRADE_TIER1_WEIGHT = 100.0f;
static const f32 TRADE_TIER2_WEIGHT = 10.0f;
static const f32 TRADE_TIER3_WEIGHT = 1.0f;
static const i32 TRADE_TIER_NEARBY_HOPS = 2;



// Base market attractiveness of an UNINHABITED system that holds stations (no civ power multiplier).

// Kept below a habited market's typical (1 + power) so trade still gravitates toward civilizations,

// while letting frontier station-markets delegate and receive a share of contracts.

static const f32 TRADE_UNINHABITED_MARKET = 1.0f;



// ---- Deterministic RNG (splitmix64) -------------------------------------------------------

static inline u64 sm64(u64& x) {

    u64 z = (x += 0x9E3779B97F4A7C15ull);

    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;

    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;

    return z ^ (z >> 31);

}

static inline u32 rnd_u32(u64& s) { return (u32)(sm64(s) >> 32); }

static inline f32 rnd_f01(u64& s) { return (f32)(rnd_u32(s) >> 8) / (f32)(1u << 24); }





// ---- Jump-point geometry --------------------------------------------------------------------

// A system's jump circle: traders must reach this ring to engage the jump drive, and they drop

// out of jump on it when arriving. Radius = 2x the furthest planet orbit of the node.

static f32 node_jump_radius(const GalaxyState& g, i32 node) {

    const GalaxyNode& nd = g.nodes[node];

    if (nd.orbit_count > 0) return 2.0f * nd.orbit_radii[nd.orbit_count - 1];

    return JUMP_RADIUS_FALLBACK;

}



// The point on `node`'s jump circle facing `toward`'s system centre: exit point when jumping

// toward it, entry point when arriving from it.

static HierPos2 node_jump_point(const GalaxyState& g, i32 node, i32 toward) {

    const GalaxyNode& nd = g.nodes[node];

    Vec2 dir = hierpos_diff(&g.nodes[toward].galaxy_center, &nd.galaxy_center, BS_HIERPOS_CELL_SIZE);

    f32 len = vec2_length(dir);

    dir = (len > 1.0f) ? vec2_scale(dir, 1.0f / len) : Vec2{ 1.0f, 0.0f };

    return hierpos_add_vec2(&nd.galaxy_center, vec2_scale(dir, node_jump_radius(g, node)));

}



// Advance m.pos toward m.leg_target at `speed` (u/s), consuming up to `hours` (== seconds at 1x).

// Returns TRUE on arrival, leaving the unspent remainder in `hours` so the caller can chain legs.

static b8 mission_move_leg(ShipMission& m, f32 speed, f32& hours) {

    if (speed <= 0.0f) { hours = 0.0f; return FALSE; }

    Vec2 d   = hierpos_diff(&m.leg_target, &m.pos, BS_HIERPOS_CELL_SIZE);

    f32 dist = vec2_length(d);

    f32 step = speed * hours;

    if (step < dist) {

        m.pos = hierpos_add_vec2(&m.pos, vec2_scale(d, step / dist));

        hours = 0.0f;

        return FALSE;

    }

    m.pos   = m.leg_target;

    hours  -= dist / speed;   // charge only the time this leg actually took

    return TRUE;

}



// Compute a route from `start` to `dest` and cache its first chunk into the mission. The full path

// is found in a large scratch buffer, then only the first MISSION_ROUTE_MAX hops are kept (long

// routes are walked in chunks, re-pathed on exhaustion). Returns FALSE if no path exists.

static b8 mission_repath(game_state* s, ShipMission& m, i32 start, i32 dest) {

    i32 full[ROUTE_SCRATCH];

    i32 full_len = 0;

    if (!galaxy_route_find(s, start, dest, full, ROUTE_SCRATCH, &full_len)) return FALSE;

    i32 keep = (full_len < MISSION_ROUTE_MAX) ? full_len : MISSION_ROUTE_MAX;

    for (i32 i = 0; i < keep; ++i) m.route[i] = full[i];

    m.route_len = keep;

    m.route_pos = 0;

    m.dest_node = dest;

    return TRUE;

}



// ---- Trade objective (OBJ_TRADE) ----------------------------------------------------------

// Debug tally of completed deliveries, logged sparsely so the objective is observably "doing work"

// without spamming. Not gameplay state (no economy field exists yet); purely diagnostic.

static i32 g_trade_deliveries = 0;



// TRUE if `node` can act as a trade market: a system controlled by an alive civilization, OR an

// uninhabited system that holds stations. Uninhabited station-markets can receive trade contracts,

// but only habited systems issue them (see ship_missions_seed).

static b8 node_is_market(game_state* s, i32 node) {

    const GalaxyState& g = s->galaxy;

    if (node < 0 || node >= g.node_count) return FALSE;

    i16 owner = g.node_owner ? g.node_owner[node] : (i16)-1;

    if (owner >= 0 && owner < g.civ_count && g.civs[owner].status == 0) return TRUE;   // habited

    StationLayoutEntry layout[SYSTEM_STATION_MAX];

    return galaxy_node_station_layout(s, node, layout, SYSTEM_STATION_MAX) > 0;         // uninhabited w/ stations

}



// A node's attractiveness as a trade market. There is no per-node economy in the world model, so

// value is derived from the two signals that exist: how habitable/productive the system is

// (best_habitability) and how strong its controlling civ is (Civilization::power). Habited systems

// scale by civ power; uninhabited station-markets get a smaller flat multiplier. Non-markets = 0.

static f32 trade_value(game_state* s, i32 node) {

    const GalaxyState& g = s->galaxy;

    if (node < 0 || node >= g.node_count) return 0.0f;

    i16 owner = g.node_owner ? g.node_owner[node] : (i16)-1;

    f32 hab   = (f32)g.nodes[node].best_habitability / 255.0f;   // 0..1

    if (owner >= 0 && owner < g.civ_count && g.civs[owner].status == 0) {

        f32 power = g.civs[owner].power;                         // ~1.0f+

        return (0.5f + hab) * (1.0f + power);

    }

    if (node_is_market(s, node)) return (0.5f + hab) * TRADE_UNINHABITED_MARKET;

    return 0.0f;

}



// Choose a market to trade with from `home`: sample a handful of owned nodes and keep the one with

// the best trade_value (scaled by a random roll so traders diverge, and boosted for markets inside

// the trader's own empire so most trade stays internal). Returns -1 if none routable.

static f32 trade_tier_weight(i16 home_owner, i16 cand_owner, i32 hops) {
    if (hops <= 0) return TRADE_TIER0_WEIGHT;                    // tier 0: intra-system
    if (home_owner >= 0 && cand_owner == home_owner) {
        if (hops <= TRADE_TIER_NEARBY_HOPS) return TRADE_TIER1_WEIGHT;  // tier 1: intra-civ (close)
        return TRADE_TIER2_WEIGHT;                                // tier 1 far: still better than foreign
    }
    if (hops <= TRADE_TIER_NEARBY_HOPS) return TRADE_TIER2_WEIGHT;      // tier 2: nearby uninhabited/foreign
    return TRADE_TIER3_WEIGHT;                                    // tier 3: inter-civ global (rare)
}

static i32 trade_pick_market(game_state* s, ShipMission& m, i32 home) {

    const GalaxyState& g = s->galaxy;

    if (g.node_count <= 1 || !g.node_owner) return -1;

    MarketGood om[GOOD_COUNT];

    station_market_get(s, m.station_id, om);

    f32 origin_price = (om[m.cargo_good].price > 0.01f) ? om[m.cargo_good].price : 0.01f;

    // Collect nearby market nodes via BFS up to 3 hops from home.
    // This ensures the sampling pool is dominated by nearby systems instead of
    // uniform-random across all ~10k nodes (which almost always picks tier-3).
    const i32 NEIGHBOR_MAX = 128;
    const i32 NEIGHBOR_HOPS = 3;
    i32 neighbors[NEIGHBOR_MAX];
    i32 neighbor_hops[NEIGHBOR_MAX];
    i32 nb_count = 0;

    const GalaxyLaneGraph& lg = g.lanes;
    if (lg.adj_start && lg.adj_neighbor) {
        // BFS from home (bounded by NEIGHBOR_MAX)
        i32 queue[NEIGHBOR_MAX];
        i32 q_hops[NEIGHBOR_MAX];
        i32 head = 0, tail = 0;
        queue[tail] = home; q_hops[tail] = 0; tail++;

        while (head < tail && nb_count < NEIGHBOR_MAX) {
            i32 node = queue[head];
            i32 hops = q_hops[head];
            head++;

            if (node != home && node_is_market(s, node)) {
                neighbors[nb_count] = node;
                neighbor_hops[nb_count] = hops;
                nb_count++;
            } else if (node == home && hops == 0 && node_is_market(s, node)) {
                // Include home node as tier-0 candidate (intra-system trade)
                neighbors[nb_count] = node;
                neighbor_hops[nb_count] = 0;
                nb_count++;
            }

            if (hops >= NEIGHBOR_HOPS) continue;

            i32 a0 = lg.adj_start[node], a1 = lg.adj_start[node + 1];
            for (i32 k = a0; k < a1 && tail < NEIGHBOR_MAX; ++k) {
                i32 nb = lg.adj_neighbor[k];
                if (nb < 0 || nb >= g.node_count) continue;
                // Check if already in queue (linear scan, NEIGHBOR_MAX is small)
                b8 found = FALSE;
                for (i32 j = 0; j < tail; ++j) { if (queue[j] == nb) { found = TRUE; break; } }
                if (found) continue;
                queue[tail] = nb; q_hops[tail] = hops + 1; tail++;
            }
        }
    }

    i32 best = -1;
    f32 best_score = -1.0f;
    i32 best_hops = -1;

    // Sample from the neighborhood pool. If the pool is large enough, pick random
    // subsets. If small, evaluate all. Always reserve 1-2 slots for random far
    // nodes to allow rare inter-civ trade.
    i32 samples = (nb_count < TRADE_MARKET_SAMPLES) ? nb_count : TRADE_MARKET_SAMPLES - 2;
    if (samples < 0) samples = 0;

    for (i32 i = 0; i < samples; ++i) {
        i32 idx = (nb_count > 0) ? (i32)(rnd_u32(m.seed) % (u32)nb_count) : 0;
        i32 cand = neighbors[idx];
        i32 hops = neighbor_hops[idx];

        // Allow cand == home for tier-0 intra-system trade (different station, same node)

        i16 cand_owner = g.node_owner[cand];
        f32 tier_w = trade_tier_weight(m.owner, cand_owner, hops);

        f32 score = trade_value(s, cand) * (0.5f + rnd_f01(m.seed));
        score *= tier_w;

        MarketGood cm[GOOD_COUNT];
        station_market_get(s, station_id_make(cand, 0), cm);
        score *= cm[m.cargo_good].price / origin_price;

        BS_LOG_INFO("Market pick: home=N%d cand=N%d tier=%d hops=%d price_ratio=%.2f score=%.1f",
                    home, cand, (i32)(tier_w == TRADE_TIER0_WEIGHT ? 0 : tier_w == TRADE_TIER1_WEIGHT ? 1 : tier_w == TRADE_TIER2_WEIGHT ? 2 : 3),
                    hops, cm[m.cargo_good].price / origin_price, score);

        if (score > best_score) { best_score = score; best = cand; best_hops = hops; }
    }

    // 1-2 random far nodes for rare inter-civ trade opportunity
    for (i32 i = 0; i < 2; ++i) {
        i32 cand = (i32)(rnd_u32(m.seed) % (u32)g.node_count);
        if (cand == home || !node_is_market(s, cand)) continue;

        i32 route[ROUTE_SCRATCH]; i32 rlen = 0;
        if (!galaxy_route_find(s, home, cand, route, ROUTE_SCRATCH, &rlen)) continue;

        i16 cand_owner = g.node_owner[cand];
        f32 tier_w = trade_tier_weight(m.owner, cand_owner, rlen);

        f32 score = trade_value(s, cand) * (0.5f + rnd_f01(m.seed));
        score *= tier_w;

        MarketGood cm[GOOD_COUNT];
        station_market_get(s, station_id_make(cand, 0), cm);
        score *= cm[m.cargo_good].price / origin_price;

        BS_LOG_INFO("Market pick: home=N%d cand=N%d tier=%d hops=%d price_ratio=%.2f score=%.1f (far)",
                    home, cand, (i32)(tier_w == TRADE_TIER0_WEIGHT ? 0 : tier_w == TRADE_TIER1_WEIGHT ? 1 : tier_w == TRADE_TIER2_WEIGHT ? 2 : 3),
                    rlen, cm[m.cargo_good].price / origin_price, score);

        if (score > best_score) { best_score = score; best = cand; best_hops = rlen; }
    }

    if (best >= 0) {
        BS_LOG_INFO("Market pick: home=N%d -> WINNER N%d hops=%d score=%.1f", home, best, best_hops, best_score);
        return best;
    }

    // Tier-ordered fallback: same-civ neighbors first (1-hop via adjacency), then any routable.
    if (lg.adj_start && lg.adj_neighbor) {
        i32 a0 = lg.adj_start[home], a1 = lg.adj_start[home + 1];
        for (i32 k = a0; k < a1; ++k) {
            i32 cand = lg.adj_neighbor[k];
            if (cand < 0 || cand == home) continue;
            if (m.owner < 0 || g.node_owner[cand] != m.owner) continue;
            if (node_is_market(s, cand)) return cand;
        }
        for (i32 k = a0; k < a1; ++k) {
            i32 cand = lg.adj_neighbor[k];
            if (cand < 0 || cand == home) continue;
            if (node_is_market(s, cand)) return cand;
        }
    }

    // Last resort: any routable same-civ, then any routable market
    i32 dummy[ROUTE_SCRATCH]; i32 dlen = 0;
    for (i32 cand = 0; cand < g.node_count; ++cand) {
        if (cand == home || m.owner < 0 || g.node_owner[cand] != m.owner) continue;
        if (galaxy_route_find(s, home, cand, dummy, ROUTE_SCRATCH, &dlen)) return cand;
    }
    for (i32 cand = 0; cand < g.node_count; ++cand) {
        if (cand == home || !node_is_market(s, cand)) continue;
        if (galaxy_route_find(s, home, cand, dummy, ROUTE_SCRATCH, &dlen)) return cand;
    }

    return -1;

}



// Issue (or re-issue) the contract of mission-hub station m.station_id: pick a market reachable

// from home, cache the station anchors, route the first chunk, and reset the stage machine to the

// origin-dock loading phase. Preserves the slot's identity fields (station_id, home_node, owner,

// seed). Returns FALSE if no market is routable (caller retries after a cooldown).

static b8 mission_issue_contract(game_state* s, ShipMission& m) {

    const GalaxyState& g = s->galaxy;

    i32 home = m.home_node;

    if (home < 0 || home >= g.node_count) return FALSE;



    // Cargo manifest: haul the origin market's biggest surplus good (highest stock vs baseline).

    // Picked BEFORE the market so trade_pick_market can favour destinations that pay well for it.

    {

        MarketGood om[GOOD_COUNT];

        station_market_get(s, m.station_id, om);

        i32 best_g = 0; f32 best_ratio = -1.0f;

        for (i32 gd = 0; gd < GOOD_COUNT; ++gd) {

            f32 ratio = (om[gd].base_stock > 1.0f) ? om[gd].stock / om[gd].base_stock : 0.0f;

            if (ratio > best_ratio || (ratio == best_ratio && om[gd].base_stock > om[best_g].base_stock)) {
                best_ratio = ratio; best_g = gd;
            }

        }

        m.cargo_good     = (u8)best_g;

        m.cargo_units    = fminf(100.0f, om[best_g].stock * 0.5f);

        // reward_credits settled below after the destination station is chosen (forward contract).

    }



    i32 market = trade_pick_market(s, m, home);

    if (market < 0) return FALSE;

    if (!mission_repath(s, m, home, market)) return FALSE;



    // Destination station: a deterministic mission hub of the market system (-1 = centre fallback).

    StationLayoutEntry layout[SYSTEM_STATION_MAX];

    i32 count = galaxy_node_station_layout(s, market, layout, SYSTEM_STATION_MAX);

    i32 hubs  = (count < MISSION_HUBS_PER_SYSTEM) ? count : MISSION_HUBS_PER_SYSTEM;

    if (hubs > 0) {

        i32 di = (i32)(rnd_u32(m.seed) % (u32)hubs);

        // Tier-0: if destination is same node as origin, pick a different station index.
        if (market == home && hubs > 1) {
            i32 origin_idx = station_id_index(m.station_id);
            if (di == origin_idx) di = (di + 1) % hubs;
        }

        m.dest_station_id     = station_id_make(market, di);

        m.dest_station_pos    = layout[di].pos;

        m.dest_station_radius = layout[di].radius;

    } else {

        m.dest_station_id     = -1;

        m.dest_station_pos    = g.nodes[market].galaxy_center;

        m.dest_station_radius = 15000.0f;

    }



    // Forward contract: settle the reward now using pre-depletion origin and destination prices.

    // This is the price spread the trader locks in at issue time — the player sees it immediately

    // in the Contracts tab, and it can't flip negative at delivery due to origin stock depletion.

    {

        MarketGood om[GOOD_COUNT], dm[GOOD_COUNT];

        station_market_get(s, m.station_id, om);

        i32 dsid = (m.dest_station_id >= 0) ? m.dest_station_id : station_id_make(market, 0);

        station_market_get(s, dsid, dm);

        m.reward_credits = m.cargo_units * (dm[m.cargo_good].price - om[m.cargo_good].price);

        if (m.reward_credits < m.cargo_units) m.reward_credits = m.cargo_units;  // min haulage fee



        // ---- Return leg: find destination's best surplus good with positive spread back to origin.

        m.return_leg         = FALSE;

        m.return_cargo_good  = (u8)GOOD_COUNT;   // sentinel: no return

        m.return_cargo_units = 0.0f;

        m.return_reward      = 0.0f;



        i32  best_ret_g  = -1;

        f32  best_ret_score = -1.0f;

        for (i32 gd = 0; gd < GOOD_COUNT; ++gd) {

            if (gd == (i32)m.cargo_good) continue;             // don't haul the same good back

            f32 dest_ratio = (dm[gd].base_stock > 1.0f) ? dm[gd].stock / dm[gd].base_stock : 0.0f;

            f32 spread     = om[gd].price - dm[gd].price;      // origin pays more than dest?

            if (spread <= 0.0f) continue;                       // not profitable

            f32 score = dest_ratio * spread;                    // surplus * profitability

            if (score > best_ret_score) { best_ret_score = score; best_ret_g = gd; }

        }

        if (best_ret_g >= 0) {

            m.return_cargo_good  = (u8)best_ret_g;

            m.return_cargo_units = fminf(100.0f, dm[best_ret_g].stock * 0.5f);

            m.return_reward      = m.return_cargo_units * (om[best_ret_g].price - dm[best_ret_g].price);

            if (m.return_reward < m.return_cargo_units) m.return_reward = m.return_cargo_units;

            BS_LOG_INFO("Contract issue: station=N%d/%d good=%s units=%.0f reward=%.0f origin_price=%.1f dest_price=%.1f | return: %s %.0f units reward=%.0f",
                        station_id_node(m.station_id), station_id_index(m.station_id),
                        TRADE_GOOD_NAMES[m.cargo_good], m.cargo_units, m.reward_credits,
                        om[m.cargo_good].price, dm[m.cargo_good].price,
                        TRADE_GOOD_NAMES[best_ret_g], m.return_cargo_units, m.return_reward);

        } else {

            BS_LOG_INFO("Contract issue: station=N%d/%d good=%s units=%.0f reward=%.0f origin_price=%.1f dest_price=%.1f",
                        station_id_node(m.station_id), station_id_index(m.station_id),
                        TRADE_GOOD_NAMES[m.cargo_good], m.cargo_units, m.reward_credits,
                        om[m.cargo_good].price, dm[m.cargo_good].price);

        }

    }



    // Origin station anchor (cached once; stations are static).

    if (!galaxy_station_pos_by_id(s, m.station_id, &m.station_pos, &m.station_radius)) {

        m.station_pos    = g.nodes[home].galaxy_center;

        m.station_radius = 15000.0f;

    }



    m.at_node   = home;

    m.from_node = home;

    m.to_node   = home;

    m.progress  = 0.0f;

    // Natural AI: the trader "acquires" the contract somewhere in the home system and first flies

    // to the issuing station. Spawn at a deterministic random point inside the jump circle

    // (sqrt for uniform area density), then fly the ACQUIRE leg at ai_speed_in_system.

    {

        f32 jr  = node_jump_radius(g, home);

        f32 ang = rnd_f01(m.seed) * 2.0f * BS_PI;

        f32 rad = sqrtf(rnd_f01(m.seed)) * jr;

        m.pos = hierpos_add_vec2(&g.nodes[home].galaxy_center, Vec2{ cosf(ang) * rad, sinf(ang) * rad });

    }

    m.leg_target    = m.station_pos;

    m.stage         = MISSION_STAGE_ACQUIRE;

    m.dwell_hours   = TRADE_DWELL_HOURS;

    m.local_ready   = FALSE;

    m.ship_slot     = -1;

    m.respawn_hours = 0.0f;

    m.active        = TRUE;

    return TRUE;

}



// Resolve a dock stage: when the player occupies the mission's system the LIVE trader decides

// (bind -> dock -> local_ready); anywhere else a macro dwell timer stands in for the docking

// pause. Consumes `hours` while waiting on the timer. Returns TRUE once docking is complete.

static b8 mission_dock_complete(ShipMission& m, i32 node_here, f32& hours) {

    if (m.at_node >= 0 && m.at_node == node_here) {

        if (m.ship_slot >= 0) return m.local_ready;   // bound: the live docking loop decides

        hours = 0.0f;                                 // unbound: wait for the closest free trader

        return FALSE;

    }

    f32 d = (hours < m.dwell_hours) ? hours : m.dwell_hours;

    m.dwell_hours -= d;

    hours         -= d;

    return (m.dwell_hours <= 0.0f);

}



// The next hop from m.at_node (repathing when the cached chunk is exhausted). Returns the next

// node index, or -1 when already at the destination or when no route currently exists.

static i32 mission_next_hop(game_state* s, ShipMission& m) {

    if (m.route_pos >= m.route_len) {

        if (m.at_node == m.dest_node) return -1;

        if (!mission_repath(s, m, m.at_node, m.dest_node)) return -1; // next chunk of a long route

        if (m.route_len <= 0) return -1;

    }

    return m.route[m.route_pos];

}



// Advance one contract by `hours` through the natural-AI stage machine: fly to the station, load,

// fly to the jump circle, jump between systems, cross intermediate systems in-system, and dock at

// the target. A bounded loop chains legs across a single tick (guards against per-frame time

// spikes). `node_here` = the player's current galaxy node (live-trader docking handoff).

static void mission_travel_step(game_state* s, ShipMission& m, f32 hours, i32 node_here) {

    GalaxyState& g = s->galaxy;

    f32 spd_sys  = (g.ai_speed_in_system > 0.0f) ? g.ai_speed_in_system : 50000.0f;

    f32 spd_jump = (g.ai_speed_jump      > 0.0f) ? g.ai_speed_jump      : 1.0e6f;

    i32 guard = 0;

    while (hours > 0.0f && guard++ < 64) {

        switch (m.stage) {



        // ---- Acquire: fly from the spawn point to the issuing station ------------------

        case MISSION_STAGE_ACQUIRE: {

            m.leg_target = m.station_pos;

            if (!mission_move_leg(m, spd_sys, hours)) return;

            m.stage       = MISSION_STAGE_ORIGIN_DOCK;

            m.dwell_hours = TRADE_DWELL_HOURS;

            m.local_ready = FALSE;

        } break;



        // ---- Dock stages: load at the origin hub / unload at the destination hub -------

        case MISSION_STAGE_ORIGIN_DOCK:

        case MISSION_STAGE_MARKET_DOCK: {

            if (!mission_dock_complete(m, node_here, hours)) return;

            if (m.stage == MISSION_STAGE_MARKET_DOCK) {

                // Delivery complete: glut the destination market (stock up -> price down).

                // Reward was already settled at contract issue time (forward contract).

                i32 dsid = (m.dest_station_id >= 0) ? m.dest_station_id : station_id_make(m.dest_node, 0);

                station_market_apply(s, dsid, m.cargo_good, m.cargo_units);

                // Station revenue: origin station earns from selling surplus at local price.
                {
                    MarketGood om[GOOD_COUNT];
                    station_market_get(s, m.station_id, om);
                    f32 rev = m.cargo_units * om[m.cargo_good].price;
                    station_revenue_add(s, m.station_id, rev);
                    BS_LOG_INFO("Station revenue: N%d/%d +%+.0f cr (export %s %.0f units @ %.1f) -> total %.0f",
                                station_id_node(m.station_id), station_id_index(m.station_id),
                                rev, TRADE_GOOD_NAMES[m.cargo_good], m.cargo_units, om[m.cargo_good].price,
                                station_revenue_get(s, m.station_id));
                }

                if ((++g_trade_deliveries % 32) == 1)

                    BS_LOG_INFO("ShipAI trade: %d deliveries (last: %.0f %s for %.0f cr)",

                                g_trade_deliveries, m.cargo_units,

                                TRADE_GOOD_NAMES[m.cargo_good], m.reward_credits);



                // ---- Return leg: if a profitable return cargo was settled at issue time, load it

                // at the destination and fly back to the origin station.

                if (m.return_cargo_units > 0.0f && m.return_cargo_good < GOOD_COUNT) {

                    // Save dsid before repath overwrites m.dest_node (Bug 2 fix).
                    if (m.dest_station_id < 0) m.dest_station_id = dsid;

                    m.return_leg = TRUE;

                    if (!mission_repath(s, m, m.at_node, m.home_node)) {

                        // No route back: fall through to cooldown.
                        m.return_leg    = FALSE;

                        m.active        = FALSE;

                        m.stage         = MISSION_STAGE_COOLDOWN;

                        m.respawn_hours = STATION_CONTRACT_COOLDOWN;

                        m.ship_slot     = -1;

                        m.local_ready   = FALSE;

                        return;

                    }

                    // Deplete destination of return cargo only after route is confirmed (Bug 1 fix).
                    station_market_apply(s, dsid, m.return_cargo_good, -m.return_cargo_units);

                    if (m.at_node == m.home_node) {

                        m.leg_target = m.station_pos;

                        m.stage      = MISSION_STAGE_FINAL_APPROACH;

                    } else {

                        i32 next = mission_next_hop(s, m);

                        if (next < 0) { m.return_leg = FALSE; m.active = FALSE; m.stage = MISSION_STAGE_COOLDOWN; m.respawn_hours = STATION_CONTRACT_COOLDOWN; m.ship_slot = -1; m.local_ready = FALSE; return; }

                        m.leg_target = node_jump_point(g, m.at_node, next);

                        m.stage      = MISSION_STAGE_TO_JUMP;

                    }

                    BS_LOG_INFO("Return leg: station=N%d/%d -> N%d good=%s units=%.0f",

                                station_id_node(m.station_id), station_id_index(m.station_id),

                                m.home_node, TRADE_GOOD_NAMES[m.return_cargo_good], m.return_cargo_units);

                    m.dwell_hours = TRADE_DWELL_HOURS;

                    m.local_ready = FALSE;

                    m.ship_slot   = -1;

                    return;

                }



                m.active        = FALSE;

                m.stage         = MISSION_STAGE_COOLDOWN;

                m.respawn_hours = STATION_CONTRACT_COOLDOWN;

                m.ship_slot     = -1;

                m.local_ready   = FALSE;

                return;

            }

            // Origin loading finished -> undock; any bound trader returns to ambient duty.

            // The cargo leaves the origin market (stock down -> price up).

            // ---- Return leg arrival: unload return cargo at origin, then cooldown.

            if (m.return_leg) {

                station_market_apply(s, m.station_id, m.return_cargo_good, m.return_cargo_units);

                {
                    MarketGood dm[GOOD_COUNT];
                    i32 dsid = (m.dest_station_id >= 0) ? m.dest_station_id : station_id_make(m.dest_node, 0);
                    station_market_get(s, dsid, dm);
                    f32 rev = m.return_cargo_units * dm[m.return_cargo_good].price;
                    station_revenue_add(s, dsid, rev);
                    BS_LOG_INFO("Station revenue: N%d/%d +%+.0f cr (return %s %.0f units @ %.1f) -> total %.0f",
                                station_id_node(dsid), station_id_index(dsid),
                                rev, TRADE_GOOD_NAMES[m.return_cargo_good], m.return_cargo_units, dm[m.return_cargo_good].price,
                                station_revenue_get(s, dsid));
                }

                BS_LOG_INFO("Return delivery: station=N%d/%d good=%s units=%.0f reward=%.0f",

                            station_id_node(m.station_id), station_id_index(m.station_id),

                            TRADE_GOOD_NAMES[m.return_cargo_good], m.return_cargo_units, m.return_reward);

                m.active        = FALSE;

                m.stage         = MISSION_STAGE_COOLDOWN;

                m.respawn_hours = STATION_CONTRACT_COOLDOWN;

                m.ship_slot     = -1;

                m.local_ready   = FALSE;

                m.return_leg    = FALSE;

                return;

            }

            station_market_apply(s, m.station_id, m.cargo_good, -m.cargo_units);

            m.ship_slot   = -1;

            m.local_ready = FALSE;

            // Tier 0 (intra-system): already at the destination node, skip jump stages.
            if (m.at_node == m.dest_node) {
                m.leg_target = m.dest_station_pos;
                m.stage      = MISSION_STAGE_FINAL_APPROACH;
                BS_LOG_INFO("Dock complete: station=N%d/%d at_node=N%d dest_node=N%d -> FINAL_APPROACH (tier 0)",
                            station_id_node(m.station_id), station_id_index(m.station_id),
                            m.at_node, m.dest_node);
                return;
            }

            i32 next = mission_next_hop(s, m);

            if (next < 0) return;                    // no routable hop: retry next tick

            m.leg_target = node_jump_point(g, m.at_node, next);

            m.stage      = MISSION_STAGE_TO_JUMP;

            BS_LOG_INFO("Dock complete: station=N%d/%d at_node=N%d dest_node=N%d -> TO_JUMP",
                        station_id_node(m.station_id), station_id_index(m.station_id),
                        m.at_node, m.dest_node);

        } break;



        // ---- In-system legs toward the exit jump-point ----------------------------------

        case MISSION_STAGE_TO_JUMP:

        case MISSION_STAGE_CROSS: {

            if (!mission_move_leg(m, spd_sys, hours)) return;

            // On the jump circle: engage the jump drive toward the next hop's entry point.

            i32 next = mission_next_hop(s, m);

            if (next < 0) return;                    // route vanished: hold at the circle, retry

            m.from_node  = m.at_node;

            m.to_node    = next;

            m.at_node    = -1;

            m.progress   = 0.0f;

            m.leg_target = node_jump_point(g, next, m.from_node);   // entry point of the target system

            m.stage      = MISSION_STAGE_JUMP;

        } break;



        // ---- Jump: between systems at jump speed ----------------------------------------

        case MISSION_STAGE_JUMP: {

            if (!mission_move_leg(m, spd_jump, hours)) return;

            m.at_node = m.to_node;

            m.route_pos++;

            if (m.at_node == m.dest_node) {

                // Final system: fly in from the jump circle and dock at the target station.

                // Return leg: target is the origin station, not the export destination.

                m.leg_target = m.return_leg ? m.station_pos : m.dest_station_pos;

                m.stage      = MISSION_STAGE_FINAL_APPROACH;

            } else {

                // Intermediate system: cross in-system to the exit jump-point of the next hop.

                i32 next = mission_next_hop(s, m);

                if (next < 0) return;                // no onward route: hold here, retry next tick

                m.leg_target = node_jump_point(g, m.at_node, next);

                m.stage      = MISSION_STAGE_CROSS;

            }

        } break;



        // ---- Final approach: jump circle -> destination station -------------------------

        case MISSION_STAGE_FINAL_APPROACH: {

            if (!mission_move_leg(m, spd_sys, hours)) return;

            m.stage       = m.return_leg ? MISSION_STAGE_ORIGIN_DOCK : MISSION_STAGE_MARKET_DOCK;

            m.dwell_hours = TRADE_DWELL_HOURS;

            m.local_ready = FALSE;

        } break;



        default: return;                             // COOLDOWN is handled by the caller

        }

    }

}



void ship_missions_seed(game_state* s) {

    GalaxyState& g = s->galaxy;

    if (!g.missions || g.mission_capacity <= 0 || g.node_count <= 1 || !g.node_owner) return;



    u64 rng = 0xA5A5A5A5DEADBEEFull ^ ((u64)g.node_count * 0x9E3779B97F4A7C15ull);

    i32 spawned = 0;

    b8 capacity_hit = FALSE;



    // One contract per mission-hub station, but ONLY habited systems ISSUE contracts (spawn traders).
    // Uninhabited stations are receive-only: they never originate a trader ship -- they are reached by
    // traders issued from habited systems (see node_is_market / trade_pick_market, which still allow
    // uninhabited station-nodes as trade DESTINATIONS). galaxy_node_station_layout returns 0 for
    // systems without stations, so those nodes contribute no hubs regardless.
    for (i32 node = 0; node < g.node_count && !capacity_hit; ++node) {

        i16 owner = g.node_owner[node];

        b8  alive = (owner >= 0 && owner < g.civ_count && g.civs[owner].status == 0);

        if (!alive) continue;                          // uninhabited: receive-only, issues no contracts

        StationLayoutEntry layout[SYSTEM_STATION_MAX];

        i32 count = galaxy_node_station_layout(s, node, layout, SYSTEM_STATION_MAX);

        if (count <= 0) continue;                      // no stations -> no contracts

        i32 hubs  = (count < MISSION_HUBS_PER_SYSTEM) ? count : MISSION_HUBS_PER_SYSTEM;

        for (i32 k = 0; k < hubs; ++k) {

            if (spawned >= g.mission_capacity) { capacity_hit = TRUE; break; }

            ShipMission& m = g.missions[spawned];

            m = ShipMission{};

            m.owner      = owner;

            m.archetype  = ARCHETYPE_TRADER;

            m.objective  = OBJ_TRADE;

            m.home_node  = node;

            m.station_id = station_id_make(node, k);

            m.ship_slot  = -1;

            m.seed       = sm64(rng) ^ ((u64)m.station_id << 24);

            if (!mission_issue_contract(s, m)) { m.active = FALSE; continue; }

            ++spawned;

        }

    }

    g.mission_count = spawned;

    if (capacity_hit)

        BS_LOG_WARN("ShipAI: mission pool full (%d); some stations issue no contracts", g.mission_capacity);

    BS_LOG_INFO("ShipAI: seeded %d station-issued trade contracts", spawned);

}



void ship_missions_update(game_state* s, f32 sim_dt_hours) {

    if (sim_dt_hours <= 0.0f) return;                 // paused

    station_markets_decay(s, sim_dt_hours);           // markets drift back toward baseline

    GalaxyState& g = s->galaxy;

    if (!g.missions) return;

    // The galaxy node the player currently occupies (current_system is a cache SLOT, not a node).

    i32 node_here = (g.current_system >= 0 && g.current_system < g.system_count)

                        ? g.cache_node[g.current_system] : -1;

    for (i32 i = 0; i < g.mission_count; ++i) {

        ShipMission& m = g.missions[i];

        if (!m.active) {

            // Cooldown: the origin station issues a fresh contract once the pause elapses.

            if (m.stage == MISSION_STAGE_COOLDOWN && m.station_id >= 0) {

                m.respawn_hours -= sim_dt_hours;

                if (m.respawn_hours <= 0.0f && !mission_issue_contract(s, m))

                    m.respawn_hours = STATION_CONTRACT_COOLDOWN;   // no routable market: retry later

            }

            continue;

        }

        mission_travel_step(s, m, sim_dt_hours, node_here);

    }

}



HierPos2 ship_mission_position(const game_state* s, const ShipMission& m) {

    (void)s;

    // Dock stages / cooldown: sit at the relevant station anchor (cached at contract issue).

    if (m.stage == MISSION_STAGE_MARKET_DOCK) return m.dest_station_pos;

    if (m.stage == MISSION_STAGE_ORIGIN_DOCK || m.stage == MISSION_STAGE_COOLDOWN)

        return m.station_pos;

    // Every moving stage reports the continuously-integrated position.

    return m.pos;

}



void ship_mission_notify_destroyed(game_state* s, i32 mission_id) {

    GalaxyState& g = s->galaxy;

    if (!g.missions || mission_id < 0 || mission_id >= g.mission_count) return;

    ShipMission& m = g.missions[mission_id];

    if (!m.active) return;

    // Contract failed: the origin station re-issues after the standard cooldown.

    m.active        = FALSE;

    m.stage         = MISSION_STAGE_COOLDOWN;

    m.respawn_hours = STATION_CONTRACT_COOLDOWN;

    m.ship_slot     = -1;

    m.local_ready   = FALSE;

    BS_LOG_INFO("ShipAI trade: mission %d destroyed in-system (contract failed; station will re-issue)", mission_id);

}



