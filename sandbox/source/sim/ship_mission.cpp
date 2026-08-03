#include "sim/ship_mission.h"

#include "game.h"              // full game_state + ShipMission definition

#include "sim/galaxy_map.h"    // galaxy_route_find, galaxy_lane_length

#include "sim/ai_ship.h"       // ShipArchetype (ARCHETYPE_TRADER)

#include "sim/station_market.h" // minimal economy: baseline markets + trader-dock deltas

#include "sim/galaxy_history.h" // garrisons + war matrix (Phase 4 military logistics)

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

// Watchdog budget: how many game-hours a mission may wait on the LOCAL tier WITHOUT MAKING
// PROGRESS before the macro tier takes its stage back. The two tiers hand stages to each other via
// `local_ready`; if a materialised hull is pinned in combat, culled mid-handshake, or a hub never
// frees a trader, the mission would wait forever. The budget is measured against progress (see
// mission_stall_check) rather than raw elapsed time, because a dock approach legitimately runs at
// the hull's own max_speed -- orders of magnitude below the macro leg speed -- and a pure timeout
// fired constantly on healthy approaches.

static const f32 MISSION_STALL_HOURS = 120.0f;

// Distance a bound hull must close on its stage target to count as progress (world units). Well
// above per-frame jitter, well below any real approach.

static const f32 MISSION_PROGRESS_EPS = 250.0f;

// Advance the stall watchdog for a mission whose current stage is owned by a live hull. `target` is
// the point that hull is trying to reach. Returns TRUE when the mission has waited out its budget
// without closing on the target, i.e. the local tier is genuinely wedged rather than merely slow.
static b8 mission_stall_check(ShipMission& m, const HierPos2& target, f32 waited) {

    f32 d = vec2_length(hierpos_diff(&target, &m.pos, BS_HIERPOS_CELL_SIZE));

    if (m.stall_ref <= 0.0f || d < m.stall_ref - MISSION_PROGRESS_EPS) {

        m.stall_ref   = d;        // closing: the hull is working, reset the budget

        m.stall_hours = 0.0f;

        return FALSE;

    }

    if (d < m.stall_ref) m.stall_ref = d;   // creeping forward; keep the tightest mark

    m.stall_hours += waited;

    return (m.stall_hours > MISSION_STALL_HOURS);

}

// Phase 5: contracts worth at least this much (both legs) buy a warship escort. Set high enough
// that an escort marks genuinely valuable cargo -- at a low threshold nearly every contract bought
// one and piracy could never land a blow.

static const f32 ESCORT_REWARD_MIN = 4000.0f;

// How much an escort cuts a raider's interception odds. Not zero: escorts deter, they do not grant
// immunity, so a determined sortie can still fight past one.

static const f32 ESCORT_ODDS_MUL = 0.30f;



// ---- Phase 6: the economic loop (trade -> wealth -> power -> fleets) -----------------------

// Deliveries credit the receiving node's owner. Every ECONOMY_TICK_HOURS each civ settles that

// income: part becomes `power` drift, part is earmarked as a military budget that PAYS for the

// Phase 4/5 missions and rebuilds hulls lost to raiders. Strangle a civ's lanes and its power,

// its fleet count and its ability to replace losses all fall together.

static const f32 ECONOMY_TICK_HOURS     = 24.0f;

static const f32 CIV_TAX_PER_NODE       = 1.5f;      // baseline territory income per settlement

static const f32 CIV_POWER_PER_CREDIT   = 5.0e-5f;   // prosperity -> logistic power drift

static const f32 CIV_POWER_GAIN_MAX     = 0.35f;     // clamp per settlement (no runaway growth)

static const f32 CIV_MIL_BUDGET_SHARE   = 0.45f;     // fraction of income earmarked for the fleet

static const f32 CIV_MIL_BUDGET_MAX     = 30000.0f;  // war chests do not grow without bound

static const f32 CIV_HULL_COST          = 1600.0f;   // credits to lay down one replacement hull

static const i16 CIV_HULL_POOL_MAX      = 12;        // ready hulls a civ can keep on the slip

static const f32 SHIPYARD_WAIT_HOURS    = 48.0f;     // recheck interval while the slips are empty



// Military missions are no longer free: each launch is paid for out of the civ's war chest.

static const f32 MISSION_COST_REINFORCE = 2200.0f;

static const f32 MISSION_COST_PATROL    = 900.0f;

static const f32 MISSION_COST_RAID      = 3000.0f;



// ...and a standing fleet costs wages every settlement. Upkeep is what makes wealth and fleet

// size an equilibrium rather than a ratchet: cut a civ's trade and it must stand ships down.

static const f32 MISSION_UPKEEP_PATROL    = 420.0f;

static const f32 MISSION_UPKEEP_REINFORCE = 520.0f;

static const f32 MISSION_UPKEEP_RAID      = 640.0f;



// Route risk: raider activity marks a node; the mark decays every economy tick. Traders route

// around marked nodes, which is what makes a sustained pirate campaign read as a blockade.

static const f32 NODE_RISK_SORTIE       = 0.40f;     // raiders launched a sortie at this hub

static const f32 NODE_RISK_AMBUSH       = 1.00f;     // a convoy was actually taken here

static const f32 NODE_RISK_DECAY        = 0.82f;     // per economy tick

static const f32 NODE_RISK_MIN          = 0.02f;     // below this an entry is released

static const f32 TRADE_RISK_WEIGHT      = 1.60f;     // how hard risk divides a destination's score

static const f32 TRADE_WAR_PENALTY      = 0.08f;     // score multiplier for a destination at war with us



// ---- Node risk table ----------------------------------------------------------------------

// Bounded and self-evicting: only nodes that have actually seen raiders occupy a slot.

static void node_risk_add(GalaxyState& g, i32 node, f32 amount) {

    if (node < 0 || amount <= 0.0f) return;

    NodeRisk* free_slot = nullptr;

    NodeRisk* weakest   = nullptr;

    for (i32 i = 0; i < NODE_RISK_MAX; ++i) {

        NodeRisk& r = g.node_risks[i];

        if (r.node == node) { r.risk += amount; return; }

        if (r.node < 0) { if (!free_slot) free_slot = &r; continue; }

        if (!weakest || r.risk < weakest->risk) weakest = &r;

    }

    NodeRisk* slot = free_slot ? free_slot : weakest;

    if (!slot) return;

    // Evicting the weakest entry is only fair if the newcomer is at least as dangerous.

    if (!free_slot && slot->risk > amount) return;

    slot->node = node;

    slot->risk = amount;

}



static f32 node_risk_get(const GalaxyState& g, i32 node) {

    if (node < 0) return 0.0f;

    for (i32 i = 0; i < NODE_RISK_MAX; ++i)

        if (g.node_risks[i].node == node) return g.node_risks[i].risk;

    return 0.0f;

}



// Public accessor: the live AI reads lane danger to decide where to post interceptors (Phase 7).

f32 ship_mission_node_risk(const game_state* s, i32 node) {

    return node_risk_get(s->galaxy, node);

}



// Credit a delivery to the civ that owns the receiving node. Unowned space banks nothing:

// nobody taxes a frontier depot.

static void civ_trade_income(game_state* s, i32 node, f32 credits) {

    GalaxyState& g = s->galaxy;

    if (node < 0 || node >= g.node_count || !g.node_owner || credits <= 0.0f) return;

    i16 owner = g.node_owner[node];

    if (owner < 0 || owner >= g.civ_count) return;

    if (g.civs[owner].status != 0) return;

    g.civs[owner].treasury += credits;

}



// Try to pay for a military mission out of the civ's war chest. FALSE = the civ cannot afford it

// this cycle, so the order is simply not given (poverty visibly thins a fleet).

static b8 civ_afford(Civilization& civ, f32 cost) {

    if (civ.mil_budget < cost) return FALSE;

    civ.mil_budget -= cost;

    return TRUE;

}



// Draw a replacement hull for a contract whose ship was destroyed. Contracts with no living owner

// are not gated (nobody's shipyard is on the hook for them).

static b8 civ_take_hull(GalaxyState& g, i16 owner) {

    if (owner < 0 || owner >= g.civ_count) return TRUE;

    Civilization& civ = g.civs[owner];

    if (civ.status != 0) return TRUE;

    if (civ.hull_pool <= 0) return FALSE;

    --civ.hull_pool;

    return TRUE;

}



// Put an unused hull back on the slips (the contract could not be re-issued after all).

static void civ_return_hull(GalaxyState& g, i16 owner) {

    if (owner < 0 || owner >= g.civ_count) return;

    if (g.civs[owner].hull_pool < CIV_HULL_POOL_MAX) ++g.civs[owner].hull_pool;

}



// Wealth buys fleet. A civ's simultaneous mission allowance scales with the war chest it can

// sustain, so a prosperous power runs several circuits at once while a raided one can barely keep

// one hull on station. Without this the budget has nowhere to go: income piles up against the cap

// and prosperity stops meaning anything.

static i32 civ_fleet_cap(const Civilization& civ, i32 base, i32 extra_max, f32 credits_per_extra) {

    i32 extra = (credits_per_extra > 0.0f) ? (i32)(civ.mil_budget / credits_per_extra) : 0;

    if (extra < 0)         extra = 0;

    if (extra > extra_max) extra = extra_max;

    return base + extra;

}



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



// Phase 2: leg advance that respects a live agent. While a materialized NpcShip owns this mission
// (ship_slot >= 0) the macro does NOT integrate — the agent flies the leg, mirrors its position
// into m.pos each frame, and raises local_ready on arrival (same handshake as docking). Consumes
// the tick's hours so a chained stage machine can't teleport past a live ship.

static b8 mission_leg_complete(ShipMission& m, f32 speed, f32& hours) {

    if (m.ship_slot >= 0) {

        f32 waited = hours;

        hours = 0.0f;

        if (!m.local_ready) {

            // Watchdog: the live hull owns this leg, but it may be pinned in combat or otherwise
            // unable to finish it. Only a hull that is NOT closing on the leg target counts as
            // stalled; drop the binding then, and ai_ships_sync_missions releases the agent next
            // frame while the macro resumes the leg itself.
            if (mission_stall_check(m, m.leg_target, waited)) {

                BS_LOG_WARN("ShipAI watchdog: leg stalled %.0fh with no progress on live agent %d (stage %u) -- resuming macro control",

                            m.stall_hours, m.ship_slot, (u32)m.stage);

                m.stall_hours = 0.0f;

                m.stall_ref   = 0.0f;

                m.ship_slot   = -1;

                m.local_ready = FALSE;

            }

            return FALSE;

        }

        m.stall_hours = 0.0f;

        m.stall_ref   = 0.0f;

        m.local_ready = FALSE;

        m.pos = m.leg_target;

        return TRUE;

    }

    return mission_move_leg(m, speed, hours);

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

        // Phase 6: shippers price in danger. A hub raiders keep hitting is avoided, and a market
        // belonging to a civ we are at war with is all but closed to us -- which is how blockades
        // and wartime scarcity emerge without anyone scripting them.
        f32 cand_risk = node_risk_get(g, cand);
        score /= (1.0f + cand_risk * TRADE_RISK_WEIGHT);
        if (m.owner >= 0 && cand_owner >= 0 && cand_owner != m.owner &&
            galaxy_history_civ_at_war(s, (i32)m.owner, (i32)cand_owner))
            score *= TRADE_WAR_PENALTY;

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

        // Same risk / wartime discount as the near pool (see above).
        score /= (1.0f + node_risk_get(g, cand) * TRADE_RISK_WEIGHT);
        if (m.owner >= 0 && cand_owner >= 0 && cand_owner != m.owner &&
            galaxy_history_civ_at_war(s, (i32)m.owner, (i32)cand_owner))
            score *= TRADE_WAR_PENALTY;

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



    // ---- Phase 5: high-value contracts buy a warship escort (abstract defense vs ambush). ----

    m.escorted = (m.reward_credits + m.return_reward >= ESCORT_REWARD_MIN);

    if (m.escorted)

        BS_LOG_INFO("ShipAI escort: contract at N%d/%d (worth %.0f) assigned a warship escort",
                    station_id_node(m.station_id), station_id_index(m.station_id),
                    m.reward_credits + m.return_reward);



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

    // Contract slots are RECYCLED in place (unlike military orders, which are reset wholesale), so
    // any per-run watchdog/battle state must be cleared explicitly or a previously stalled slot
    // would start its next contract already over budget and refuse the live tier.
    m.stall_hours   = 0.0f;

    m.stall_ref     = 0.0f;

    m.raid_engaged  = FALSE;

    m.active        = TRUE;

    return TRUE;

}



// Resolve a dock stage: when the player occupies the mission's system the LIVE trader decides

// (bind -> dock -> local_ready); anywhere else a macro dwell timer stands in for the docking

// pause. Consumes `hours` while waiting on the timer. Returns TRUE once docking is complete.

static b8 mission_dock_complete(ShipMission& m, i32 node_here, f32& hours) {

    // Live handoff, but watchdog-guarded. Two ways this stage can wait forever: a bound hull that
    // never reports in (pinned in combat, or culled mid-handshake), and an unbound contract at a
    // hub where no free trader ever appears. Past MISSION_STALL_HOURS the mission stops waiting on
    // the local tier and falls through to the macro dwell timer below, so it always makes progress.
    if (m.at_node >= 0 && m.at_node == node_here && m.stall_hours <= MISSION_STALL_HOURS) {

        f32 waited = hours;

        if (m.ship_slot >= 0) {

            hours = 0.0f;

            if (m.local_ready) { m.stall_hours = 0.0f; m.stall_ref = 0.0f; return TRUE; }

            // Progress here is measured against the dock anchor the live hull is flying to.
            const HierPos2& anchor = (m.stage == MISSION_STAGE_MARKET_DOCK) ? m.dest_station_pos
                                                                            : m.station_pos;

            if (mission_stall_check(m, anchor, waited)) {

                BS_LOG_WARN("ShipAI watchdog: dock stalled %.0fh with no progress on live agent %d (stage %u) -- resuming macro control",

                            m.stall_hours, m.ship_slot, (u32)m.stage);

                m.stall_ref   = 0.0f;

                m.ship_slot   = -1;

                m.local_ready = FALSE;

            }

            return FALSE;

        }

        hours = 0.0f;                                 // unbound: wait for the closest free trader

        m.stall_hours += waited;

        return FALSE;

    }

    f32 d = (hours < m.dwell_hours) ? hours : m.dwell_hours;

    m.dwell_hours -= d;

    hours         -= d;

    if (m.dwell_hours <= 0.0f) { m.stall_hours = 0.0f; m.stall_ref = 0.0f; return TRUE; }

    return FALSE;

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



// ---- Phase 4+5: military logistics & raids (OBJ_REINFORCE + OBJ_PATROL + OBJ_RAID) ---------------

// A throttled per-civ survey turns garrisons into REAL ship movements: surplus systems launch

// reinforcement columns toward understrength/frontier systems (strength debited at launch,

// credited on arrival -- a column destroyed en route is strength lost forever), and strong

// systems field patrol circuits that wander the civ's territory. Both ride the ordinary mission

// stage machine, so Phase 2 materialises them as live, killable warships near the player.

static const f32 MILITARY_TICK_HOURS   = 6.0f;   // survey cadence (game hours)

static const i32 MIL_REINFORCE_PER_CIV = 2;      // max simultaneous columns per civ

static const i32 MIL_PATROL_PER_CIV    = 2;      // max simultaneous patrol circuits per civ

static const i32 MIL_SURPLUS_MIN       = 10;     // a node needs this much garrison to give any away

static const i32 MIL_WEAK_MAX          = 5;      // nodes at/below this strength want reinforcement

static const i32 MIL_PATROL_HOPS       = 6;      // circuit length before a patrol retires

// ---- Phase 5: raids & piracy --------------------------------------------------------------

static const i32 MIL_RAID_PER_CIV  = 1;      // max simultaneous war raids per civ

static const i32 PIRATE_RAIDS_MAX  = 24;     // galaxy-wide cap on active pirate sorties

static const i32 PIRATE_SORTIES_PER_TICK = 3; // new sorties launched per military survey

static const i32 PIRATE_PREY_HOPS  = 3;      // how far raiders will range from their den

static const f32 RAID_INTERCEPT_BASE      = 0.65f;  // ambush odds vs a convoy at the target node

static const f32 RAID_INTERCEPT_PATROLLED = 0.20f;  // ...when a patrol circuit holds that node

// How long (game hours) a raid stays on station fighting when the player is in-system. 1 real
// second == 1 game hour at 1x, so this is a battle you can actually watch (and join) rather than
// a sub-second blink. It ends early if the raiders are destroyed.
static const f32 RAID_BATTLE_HOURS        = 36.0f;




// A free mission slot for a military order: prefer retired military slots (station_id < 0, never

// re-issued by a station), else extend the high-water mark. -1 when the pool is exhausted.

// Where a military mission actually ARRIVES in its destination system. Never the star: aiming at
// galaxy_center flew raiders and columns straight into the middle of the star, where they sat and
// then blinked out as the mission resolved. Prefer a station hub -- the same anchors traders dock
// at, so every hull shares one movement philosophy -- and fall back to a standoff point on the
// innermost orbit ring for systems with no stations.
static void military_arrival_anchor(game_state* s, i32 node, u64& rng,
                                    HierPos2* out_pos, f32* out_radius) {

    const GalaxyState& g = s->galaxy;

    StationLayoutEntry layout[SYSTEM_STATION_MAX];

    i32 count = galaxy_node_station_layout(s, node, layout, SYSTEM_STATION_MAX);

    if (count > 0) {

        i32 di = (i32)(sm64(rng) % (u64)count);

        *out_pos    = layout[di].pos;

        *out_radius = layout[di].radius;

        return;

    }

    const GalaxyNode& dn = g.nodes[node];

    f32 ring = (dn.orbit_count > 0) ? dn.orbit_radii[0] : 60000.0f;

    f32 ang  = (f32)(sm64(rng) % 6283ull) * 0.001f;

    *out_pos    = hierpos_add_vec2(&dn.galaxy_center, Vec2{ cosf(ang) * ring, sinf(ang) * ring });

    *out_radius = 0.0f;

}



static i32 mission_alloc_military(GalaxyState& g) {

    for (i32 i = 0; i < g.mission_count; ++i)

        if (!g.missions[i].active && g.missions[i].station_id < 0) return i;

    if (g.mission_count < g.mission_capacity) return g.mission_count++;

    return -1;

}



// Common military mission body: route src -> dst and depart from src's jump circle.

static b8 mission_issue_military(game_state* s, i32 mi, i16 civ, u8 objective, i32 src, i32 dst, f32 strength, u64& rng) {
    GalaxyState& g = s->galaxy;

    ShipMission& m = g.missions[mi];

    m = ShipMission{};

    m.owner       = civ;

    m.archetype   = ARCHETYPE_WARSHIP;

    m.objective   = objective;

    m.home_node   = src;

    m.at_node     = src;

    m.station_id  = -1;                       // not station-owned: never re-issued on cooldown

    m.dest_station_id = -1;

    m.ship_slot   = -1;

    m.cargo_units = strength;                 // strength payload (reinforce) / column size (patrol)

    m.seed        = sm64(rng);

    m.pos         = g.nodes[src].galaxy_center;

    m.dwell_hours = (f32)MIL_PATROL_HOPS;     // patrol: remaining hops (unused by reinforce)

    if (!mission_repath(s, m, src, dst)) return FALSE;

    i32 next = mission_next_hop(s, m);

    if (next < 0) return FALSE;

    m.leg_target        = node_jump_point(g, src, next);

    // Military arrivals must fly to something REAL, not to the star. The original cut aimed at
    // g.nodes[dst].galaxy_center, so raiders and columns flew into the middle of the star and
    // vanished at its centre when the mission resolved. Prefer a station hub -- the same anchors
    // traders dock at, keeping one movement philosophy for every hull -- and fall back to a
    // standoff point out on the innermost orbit ring for systems with no stations.
    military_arrival_anchor(s, dst, rng, &m.dest_station_pos, &m.dest_station_radius);

    m.stage  = MISSION_STAGE_TO_JUMP;

    m.active = TRUE;

    return TRUE;

}



// The survey: one pass over the map collects, per civ, its strongest surplus node and its

// neediest (frontier-weighted) weak node, then issues orders within the per-civ budgets.

static void ship_missions_military_tick(game_state* s) {

    GalaxyState& g = s->galaxy;

    if (!g.missions || !g.node_owner || g.civ_count <= 0) return;

    // Deterministic military/piracy RNG stream. It must be re-derived whenever the GALAXY changes:
    // previously it was seeded once per process, so starting a second galaxy in the same session
    // inherited the first one's evolved stream and the macro sim stopped being reproducible from
    // its seed. Tracking the source seed makes every galaxy replay identically, and the explicit
    // zero guard keeps the stream out of splitmix64's fixed point.
    static u64 mil_rng     = 0;

    static u64 mil_rng_src = 0;

    static b8  mil_rng_set = FALSE;

    if (!mil_rng_set || mil_rng_src != g.galaxy_seed) {

        mil_rng_src = g.galaxy_seed;

        mil_rng     = g.galaxy_seed ^ 0x5741525348495053ull;   // "WARSHIPS"

        if (mil_rng == 0) mil_rng = 0x5741525348495053ull;

        mil_rng_set = TRUE;

    }



    i32 civs = (g.civ_count < GALAXY_CIV_MAX) ? g.civ_count : GALAXY_CIV_MAX;

    static i16 rein_n [GALAXY_CIV_MAX];  static i16 pat_n  [GALAXY_CIV_MAX];

    static i32 src    [GALAXY_CIV_MAX];  static i32 src_gar[GALAXY_CIV_MAX];

    static i32 dst    [GALAXY_CIV_MAX];  static i32 dst_sc [GALAXY_CIV_MAX];

    static i16 raid_n   [GALAXY_CIV_MAX];  static i32 raid_seen[GALAXY_CIV_MAX];

    static i32 raid_src [GALAXY_CIV_MAX];  static i32 raid_dst [GALAXY_CIV_MAX];

    for (i32 c = 0; c < civs; ++c) {

        rein_n[c] = pat_n[c] = raid_n[c] = 0;  src[c] = dst[c] = -1;  src_gar[c] = 0;  dst_sc[c] = 0;

        raid_src[c] = raid_dst[c] = -1;  raid_seen[c] = 0;

    }

    i32 pirate_raids = 0;



    // Budget usage: count live military missions per civ (and pirate sorties galaxy-wide).

    for (i32 i = 0; i < g.mission_count; ++i) {

        const ShipMission& m = g.missions[i];

        if (!m.active) continue;

        if (m.owner < 0) { if (m.objective == OBJ_RAID) ++pirate_raids; continue; }

        if (m.owner >= civs) continue;

        if (m.objective == OBJ_REINFORCE) ++rein_n[m.owner];

        else if (m.objective == OBJ_PATROL) ++pat_n[m.owner];

        else if (m.objective == OBJ_RAID) ++raid_n[m.owner];

    }



    // Map survey: strongest surplus + weakest (frontier-first) node per civ.

    const GalaxyLaneGraph& lg = g.lanes;

    for (i32 node = 0; node < g.node_count; ++node) {

        i16 owner = g.node_owner[node];

        if (owner < 0 || owner >= civs || g.civs[owner].status != 0) continue;

        i32 gar = galaxy_history_garrison_at(s, node);

        if (gar >= MIL_SURPLUS_MIN && gar > src_gar[owner]) { src[owner] = node; src_gar[owner] = gar; }

        if (gar <= MIL_WEAK_MAX) {

            b8 frontier = FALSE;

            if (lg.adj_start && lg.adj_neighbor) {

                for (i32 a = lg.adj_start[node]; a < lg.adj_start[node + 1] && !frontier; ++a) {

                    i16 no = g.node_owner[lg.adj_neighbor[a]];

                    if (no != owner && (no < 0 || galaxy_history_civ_at_war(s, owner, no))) frontier = TRUE;

                }

            }

            i32 score = (frontier ? 100 : 0) + (MIL_WEAK_MAX - gar) + 1;

            if (score > dst_sc[owner]) { dst_sc[owner] = score; dst[owner] = node; }

        }

        // Phase 5: raid staging -- an owned node (with troops to spare) bordering an enemy at war.

        if (gar >= 4 && lg.adj_start && lg.adj_neighbor) {

            for (i32 a = lg.adj_start[node]; a < lg.adj_start[node + 1]; ++a) {

                i32 nb = lg.adj_neighbor[a];

                i16 no = g.node_owner[nb];

                if (no < 0 || no == owner || no >= civs) continue;

                if (!galaxy_history_civ_at_war(s, owner, no)) continue;

                ++raid_seen[owner];

                if ((sm64(mil_rng) % (u64)raid_seen[owner]) == 0) { raid_src[owner] = node; raid_dst[owner] = nb; }

            }

        }

    }



    // Issue orders within budget.

    for (i32 c = 0; c < civs; ++c) {

        if (g.civs[c].status != 0) continue;

        // Phase 6: how many simultaneous orders this civ can sustain is a function of its wealth.

        i32 rein_cap = civ_fleet_cap(g.civs[c], MIL_REINFORCE_PER_CIV, 3, 6000.0f);

        i32 pat_cap  = civ_fleet_cap(g.civs[c], MIL_PATROL_PER_CIV,    4, 4000.0f);

        i32 raid_cap = civ_fleet_cap(g.civs[c], MIL_RAID_PER_CIV,      2, 9000.0f);

        // Phase 6: columns are paid for out of the war chest. A civ whose trade has been strangled
        // simply cannot field one this cycle (civ_afford debits only when it succeeds).
        if (src[c] >= 0 && rein_n[c] < rein_cap && dst[c] >= 0 && dst[c] != src[c]

            && civ_afford(g.civs[c], MISSION_COST_REINFORCE)) {

            i32 strength = src_gar[c] / 3;

            if (strength < 2) strength = 2;

            if (strength > 8) strength = 8;

            i32 mi = mission_alloc_military(g);

            if (mi >= 0 && mission_issue_military(s, mi, (i16)c, OBJ_REINFORCE, src[c], dst[c], (f32)strength, mil_rng)) {

                galaxy_history_garrison_add(s, src[c], -strength);

                BS_LOG_INFO("ShipAI military: civ %d launches reinforcement column N%d -> N%d (strength %d, war chest %.0f cr)",

                            c, src[c], dst[c], strength, g.civs[c].mil_budget);

            } else {

                if (mi >= 0) g.missions[mi].active = FALSE;

                g.civs[c].mil_budget += MISSION_COST_REINFORCE;   // order never sailed: refund

            }

        }

        if (src[c] >= 0 && pat_n[c] < pat_cap && lg.adj_start && lg.adj_neighbor

            && civ_afford(g.civs[c], MISSION_COST_PATROL)) {

            // Patrol: wander to a random owned neighbour of the strong node.

            i32 a0 = lg.adj_start[src[c]], a1 = lg.adj_start[src[c] + 1];

            i32 pick = -1, seen = 0;

            for (i32 a = a0; a < a1; ++a) {

                i32 nb = lg.adj_neighbor[a];

                if (g.node_owner[nb] != (i16)c) continue;

                ++seen;

                if ((sm64(mil_rng) % (u64)seen) == 0) pick = nb;

            }

            b8 sailed = FALSE;

            if (pick >= 0) {

                i32 mi = mission_alloc_military(g);

                if (mi >= 0 && mission_issue_military(s, mi, (i16)c, OBJ_PATROL, src[c], pick, 2.0f, mil_rng)) {

                    sailed = TRUE;

                    BS_LOG_INFO("ShipAI military: civ %d fields a patrol circuit from N%d (%d hops, war chest %.0f cr)",

                                c, src[c], MIL_PATROL_HOPS, g.civs[c].mil_budget);

                } else if (mi >= 0) g.missions[mi].active = FALSE;

            }

            if (!sailed) g.civs[c].mil_budget += MISSION_COST_PATROL;   // no circuit sailed: refund

        }

        // Phase 5: war raid -- strike an enemy border node; troops committed are debited at launch.

        if (raid_n[c] < raid_cap && raid_src[c] >= 0) {

            i32 sgar = galaxy_history_garrison_at(s, raid_src[c]);

            i32 strength = sgar / 3;

            if (strength < 2) strength = 2;

            if (strength > 6) strength = 6;

            if (sgar > strength && civ_afford(g.civs[c], MISSION_COST_RAID)) {

                i32 mi = mission_alloc_military(g);

                if (mi >= 0 && mission_issue_military(s, mi, (i16)c, OBJ_RAID, raid_src[c], raid_dst[c], (f32)strength, mil_rng)) {

                    galaxy_history_garrison_add(s, raid_src[c], -strength);

                    BS_LOG_INFO("ShipAI raid: civ %d launches raid N%d -> enemy N%d (strength %d, war chest %.0f cr)",

                                c, raid_src[c], raid_dst[c], strength, g.civs[c].mil_budget);

                } else {

                    if (mi >= 0) g.missions[mi].active = FALSE;

                    g.civs[c].mil_budget += MISSION_COST_RAID;   // raid never launched: refund

                }

            }

        }

    }



    // ---- Phase 5: pirate sorties -- wild nodes with pirate presence strike nearby trade hubs. ----

    // Raiders hit what is within reach of their den. Picking prey galaxy-wide (the original cut)
    // produced sorties dozens of jumps long that effectively never arrived, so the cap filled with
    // permanently in-flight missions and piracy stalled. Prey is now chosen by a bounded BFS out to
    // PIRATE_PREY_HOPS, so sorties land, resolve, free their slot, and the cycle repeats.

    for (i32 sortie = 0; sortie < PIRATE_SORTIES_PER_TICK && pirate_raids < PIRATE_RAIDS_MAX; ++sortie) {

        // Reservoir-pick a pirate-infested wild node (every wild system carries a crew).

        i32 wild = -1, wseen = 0;

        for (i32 node = 0; node < g.node_count; ++node) {

            if (g.node_owner[node] >= 0) continue;

            ++wseen;

            if ((sm64(mil_rng) % (u64)wseen) == 0) wild = node;

        }

        if (wild < 0) break;

        // Bounded BFS from the den: collect owned market nodes within PIRATE_PREY_HOPS and
        // reservoir-pick one. Risky hubs are weighted up -- raiders return to a bleeding lane.

        i32 target = -1, tseen = 0;

        {

            const GalaxyLaneGraph& lg = g.lanes;

            const i32 BFS_MAX = 96;

            i32 queue[BFS_MAX], hops[BFS_MAX];

            i32 head = 0, tail = 0;

            queue[tail] = wild; hops[tail] = 0; ++tail;

            while (head < tail && lg.adj_start && lg.adj_neighbor) {

                i32 node = queue[head]; i32 h = hops[head]; ++head;

                if (node != wild && g.node_owner[node] >= 0 && node_is_market(s, node)) {

                    i32 weight = 1 + (i32)(node_risk_get(g, node) * 2.0f);   // bleeding lanes attract raiders

                    for (i32 w = 0; w < weight; ++w) {

                        ++tseen;

                        if ((sm64(mil_rng) % (u64)tseen) == 0) target = node;

                    }

                }

                if (h >= PIRATE_PREY_HOPS) continue;

                for (i32 a = lg.adj_start[node]; a < lg.adj_start[node + 1] && tail < BFS_MAX; ++a) {

                    i32 nb = lg.adj_neighbor[a];

                    if (nb < 0 || nb >= g.node_count) continue;

                    b8 seen_already = FALSE;

                    for (i32 q = 0; q < tail; ++q) if (queue[q] == nb) { seen_already = TRUE; break; }

                    if (seen_already) continue;

                    queue[tail] = nb; hops[tail] = h + 1; ++tail;

                }

            }

        }

        if (target < 0 || target == wild) continue;

        i32 strength = 2 + (i32)(sm64(mil_rng) % 3ull);

        i32 mi = mission_alloc_military(g);

        if (mi >= 0 && mission_issue_military(s, mi, FACTION_PIRATE, OBJ_RAID, wild, target, (f32)strength, mil_rng)) {

            // Pirate sorties fly as raiders, not as civ line warships: raider hull, pirate profile.
            g.missions[mi].archetype = ARCHETYPE_PIRATE;

            ++pirate_raids;

            // Word gets out. Shippers start pricing this hub as dangerous even before the raiders
            // arrive, so a hunted corridor sheds traffic (see trade_pick_market).
            node_risk_add(g, target, NODE_RISK_SORTIE);

            BS_LOG_INFO("ShipAI piracy: raiders sortie from wild N%d toward trade hub N%d (strength %d, hub risk %.2f, %d/%d active)",

                        wild, target, strength, node_risk_get(g, target), pirate_raids, PIRATE_RAIDS_MAX);

        } else if (mi >= 0) g.missions[mi].active = FALSE;

    }

}



// ---- Phase 6: civ economy settlement --------------------------------------------------------

// Runs every ECONOMY_TICK_HOURS. Converts the trade income banked by civ_trade_income (plus a

// baseline territorial tax) into logistic power drift and a military war chest, lays down

// replacement hulls for contracts lost to raiders, and lets raider heat on trade nodes decay.

// This is the loop that makes commerce, prosperity and fleet strength one system instead of three.

static void ship_missions_economy_tick(game_state* s) {

    GalaxyState& g = s->galaxy;

    if (g.civ_count <= 0) return;

    i32 civs = (g.civ_count < GALAXY_CIV_MAX) ? g.civ_count : GALAXY_CIV_MAX;

    f32 total_income = 0.0f, total_budget = 0.0f;

    i32 funded_civs = 0;

    for (i32 c = 0; c < civs; ++c) {

        Civilization& civ = g.civs[c];

        if (civ.status != 0) continue;

        f32 tax    = (f32)civ.territory_count * CIV_TAX_PER_NODE;

        f32 income = civ.treasury + tax;

        civ.treasury = 0.0f;

        if (income <= 0.0f) continue;

        ++funded_civs;

        total_income += income;

        // Prosperity grows the polity, bounded so one fat quarter cannot run away with the galaxy.

        f32 gain = income * CIV_POWER_PER_CREDIT;

        if (gain > CIV_POWER_GAIN_MAX) gain = CIV_POWER_GAIN_MAX;

        civ.power += gain;

        civ.mil_budget += income * CIV_MIL_BUDGET_SHARE;

        if (civ.mil_budget > CIV_MIL_BUDGET_MAX) civ.mil_budget = CIV_MIL_BUDGET_MAX;

    }

    // ---- Fleet upkeep: a standing fleet costs money every settlement ------------------------

    // This is what turns wealth and fleet size into an equilibrium instead of a ratchet. A civ

    // whose trade has been cut cannot pay its patrols and must stand them down -- which in turn

    // makes its lanes easier to raid. Columns and raid parties recalled this way return their

    // troops to the node they set out from.

    i32 stood_down = 0;

    for (i32 i = 0; i < g.mission_count; ++i) {

        ShipMission& m = g.missions[i];

        if (!m.active || m.owner < 0 || m.owner >= g.civ_count) continue;   // pirates pay no wages

        f32 upkeep = (m.objective == OBJ_PATROL)    ? MISSION_UPKEEP_PATROL

                   : (m.objective == OBJ_REINFORCE) ? MISSION_UPKEEP_REINFORCE

                   : (m.objective == OBJ_RAID)      ? MISSION_UPKEEP_RAID : 0.0f;

        if (upkeep <= 0.0f) continue;

        Civilization& civ = g.civs[m.owner];

        if (civ.mil_budget >= upkeep) { civ.mil_budget -= upkeep; continue; }

        if (m.objective != OBJ_PATROL)

            galaxy_history_garrison_add(s, m.home_node, (i32)m.cargo_units);   // troops march home

        BS_LOG_INFO("ShipAI economy: civ %d cannot pay for its %s -- stood down at N%d (war chest %.0f cr)",

                    (i32)m.owner,

                    (m.objective == OBJ_PATROL) ? "patrol circuit"

                      : (m.objective == OBJ_REINFORCE) ? "reinforcement column" : "raid party",

                    m.at_node, civ.mil_budget);

        m.active = FALSE; m.stage = MISSION_STAGE_COOLDOWN;

        m.ship_slot = -1; m.local_ready = FALSE;

        ++stood_down;

    }

    // ---- Shipyards: whatever survives upkeep can lay down replacement hulls -----------------

    i32 hulls_built = 0;

    for (i32 c = 0; c < civs; ++c) {

        Civilization& civ = g.civs[c];

        if (civ.status != 0) continue;

        total_budget += civ.mil_budget;

        // At most one hull per settlement, paid out of the same war chest that funds missions:

        // a civ under sustained raiding cannot both patrol and replace its losses.

        if (civ.hull_pool < CIV_HULL_POOL_MAX && civ.mil_budget >= CIV_HULL_COST) {

            civ.mil_budget -= CIV_HULL_COST;

            ++civ.hull_pool;

            ++hulls_built;

        }

    }

    // Raider heat fades wherever the raiders have moved on.

    i32 risky = 0;

    for (i32 i = 0; i < NODE_RISK_MAX; ++i) {

        NodeRisk& r = g.node_risks[i];

        if (r.node < 0) continue;

        r.risk *= NODE_RISK_DECAY;

        if (r.risk < NODE_RISK_MIN) { r.node = -1; r.risk = 0.0f; }

        else ++risky;

    }

    static i32 econ_ticks = 0;

    if ((++econ_ticks % 8) == 1) {

        i32 top = -1;

        for (i32 c = 0; c < civs; ++c) {

            if (g.civs[c].status != 0) continue;

            if (top < 0 || g.civs[c].mil_budget > g.civs[top].mil_budget) top = c;

        }

        if (top >= 0)

            BS_LOG_INFO("ShipAI economy: tick %d -- %d civs earned %.0f cr, war chests %.0f cr, %d hull(s) laid down, %d ship(s) stood down, %d risky node(s); richest %s (power %.1f, budget %.0f, hulls %d)",

                        econ_ticks, funded_civs, total_income, total_budget, hulls_built, stood_down, risky,

                        g.civs[top].name, g.civs[top].power, g.civs[top].mil_budget, (i32)g.civs[top].hull_pool);

    }

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

            if (!mission_leg_complete(m, spd_sys, hours)) return;

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
                    // Phase 6: the exporting civ banks the sale -- this is the income that later
                    // becomes power drift, military budget and replacement hulls.
                    civ_trade_income(s, station_id_node(m.station_id), rev);
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

                    m.local_ready = FALSE;   // ship_slot kept: a live trader flies the return leg itself

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
                    civ_trade_income(s, station_id_node(dsid), rev);
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

            m.local_ready = FALSE;   // ship_slot kept: a bound trader departs as the live traveler

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

            if (!mission_leg_complete(m, spd_sys, hours)) return;

            // On the jump circle: engage the jump drive toward the next hop's entry point.

            i32 next = mission_next_hop(s, m);

            if (next < 0) return;                    // route vanished: hold at the circle, retry

            m.ship_slot  = -1;   // interstellar: any live agent is despawned by the sync pass

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

            // A raid that arrived while the player is in the target system is NOT settled on paper:
            // the raiders and the defending garrison are both live hulls on screen, so the fight
            // decides it. Hold the mission here while the battle rages; if the raid party is
            // destroyed, notify_destroyed retires it and no garrison damage is ever applied.
            if (m.raid_engaged) {

                m.dwell_hours -= hours;

                hours = 0.0f;

                if (m.dwell_hours > 0.0f && m.at_node == node_here) return;   // still shooting

                m.raid_engaged = FALSE;   // battle timed out (or the player left): settle the rest

            } else {

                if (!mission_leg_complete(m, spd_sys, hours)) return;

                if (m.objective == OBJ_RAID && m.at_node == node_here && m.ship_slot >= 0) {

                    m.raid_engaged = TRUE;

                    m.dwell_hours  = RAID_BATTLE_HOURS;

                    BS_LOG_INFO("ShipAI raid: raiders reached N%d with the player present -- live battle (owner %d, strength %.0f)",

                                m.at_node, (i32)m.owner, m.cargo_units);

                    return;

                }

            }

            // ---- Phase 4: military arrivals resolve here (no dock stages) --------------

            if (m.objective == OBJ_REINFORCE) {

                galaxy_history_garrison_add(s, m.at_node, (i32)m.cargo_units);

                BS_LOG_INFO("ShipAI military: reinforcement column arrived N%d (+%d strength, civ %d)",

                            m.at_node, (i32)m.cargo_units, (i32)m.owner);

                m.active = FALSE; m.stage = MISSION_STAGE_COOLDOWN;

                m.ship_slot = -1; m.local_ready = FALSE;

                return;

            }

            if (m.objective == OBJ_PATROL) {

                m.dwell_hours -= 1.0f;               // one circuit hop done (hops ride dwell_hours)

                const GalaxyLaneGraph& lg = g.lanes;

                i32 pick = -1, seen = 0;

                // Never immediately double back on the node we just came from: a two-node ping-pong
                // reads as a broken ship rather than a patrol circuit. The previous hop is only
                // reconsidered when it is the sole owned neighbour (a dead-end picket).
                i32 came_from = m.home_node;

                if (m.dwell_hours > 0.0f && lg.adj_start && lg.adj_neighbor && g.node_owner) {

                    for (i32 pass = 0; pass < 2 && pick < 0; ++pass) {

                        seen = 0;

                        for (i32 a = lg.adj_start[m.at_node]; a < lg.adj_start[m.at_node + 1]; ++a) {

                            i32 nb = lg.adj_neighbor[a];

                            if (g.node_owner[nb] != m.owner) continue;

                            if (pass == 0 && nb == came_from) continue;   // first pass: keep moving on

                            ++seen;

                            if ((sm64(m.seed) % (u64)seen) == 0) pick = nb;

                        }

                    }

                }

                if (pick < 0 || !mission_repath(s, m, m.at_node, pick)) {

                    BS_LOG_INFO("ShipAI military: patrol circuit completed at N%d (civ %d)", m.at_node, (i32)m.owner);

                    m.active = FALSE; m.stage = MISSION_STAGE_COOLDOWN;

                    m.ship_slot = -1; m.local_ready = FALSE;

                    return;

                }

                i32 next = mission_next_hop(s, m);

                if (next < 0) return;

                m.home_node        = m.at_node;               // remember this hop as "came from"

                m.leg_target       = node_jump_point(g, m.at_node, next);

                { f32 unused_r; military_arrival_anchor(s, pick, m.seed, &m.dest_station_pos, &unused_r); }

                m.stage            = MISSION_STAGE_TO_JUMP;   // any bound live agent just keeps flying

                break;

            }

            // ---- Phase 5: raids resolve here (abstract but consequential) ---------------

            if (m.objective == OBJ_RAID) {

                if (m.owner >= 0) {

                    // Civ war strike: committed strength vs the defending garrison.

                    i32 atk = (i32)m.cargo_units;

                    i32 gar = galaxy_history_garrison_at(s, m.at_node);

                    i32 dmg = atk / 2 + (i32)(sm64(m.seed) % (u64)(atk + 1));

                    if (dmg > gar) dmg = gar;

                    if (dmg > 0) galaxy_history_garrison_add(s, m.at_node, -dmg);

                    BS_LOG_INFO("ShipAI raid: civ %d raid struck N%d (garrison %d -> %d, %s)",

                                (i32)m.owner, m.at_node, gar, gar - dmg,

                                (gar - dmg < atk) ? "raiders withdraw victorious" : "raiders repelled");

                } else {

                    // Pirate ambush at a trade hub: try to intercept a co-located contract.

                    b8 patrolled = FALSE;

                    for (i32 i = 0; i < g.mission_count && !patrolled; ++i) {

                        const ShipMission& p = g.missions[i];

                        if (p.active && p.objective == OBJ_PATROL && p.at_node == m.at_node) patrolled = TRUE;

                    }

                    // Reservoir-pick an abstract (non-materialized) trade contract this raid can

                    // actually catch: one sitting at the hub, or one INBOUND to it. A blockade

                    // catches the traffic bound for the hub, not only whatever happens to be

                    // parked there the instant the raiders arrive -- matching only the latter left

                    // raiders reporting "no prey" almost every sortie.

                    i32 victim = -1, seen = 0;

                    for (i32 i = 0; i < g.mission_count; ++i) {

                        const ShipMission& t = g.missions[i];

                        if (!t.active || t.objective != OBJ_TRADE || t.ship_slot >= 0) continue;

                        b8 here    = (t.at_node   == m.at_node);

                        b8 inbound = (t.dest_node == m.at_node);

                        if (!here && !inbound) continue;

                        i32 weight = here ? 3 : 1;      // a hull already at the hub is the easier mark

                        for (i32 w = 0; w < weight; ++w) {

                            ++seen;

                            if ((sm64(m.seed) % (u64)seen) == 0) victim = i;

                        }

                    }

                    // Escorts deter raiders, they do not make a convoy invincible: a strong sortie
                    // can still fight past one. Folding the escort into the odds (rather than
                    // treating it as an automatic save) is what keeps piracy a real threat to
                    // high-value trade instead of theatre.
                    f32 odds = patrolled ? RAID_INTERCEPT_PATROLLED : RAID_INTERCEPT_BASE;

                    b8  escorted = (victim >= 0) && g.missions[victim].escorted;

                    if (escorted) odds *= ESCORT_ODDS_MUL;

                    if (victim >= 0 && (f32)(sm64(m.seed) % 1000ull) < odds * 1000.0f) {

                        ShipMission& t = g.missions[victim];

                        t.active = FALSE; t.stage = MISSION_STAGE_COOLDOWN;

                        t.respawn_hours = STATION_CONTRACT_COOLDOWN;

                        t.ship_slot = -1; t.local_ready = FALSE;

                        // Phase 6: the hull and crew are gone. The station cannot simply re-issue
                        // -- its civ must lay down a replacement first -- and the corridor is now
                        // marked dangerous for every other shipper.
                        t.awaiting_hull = TRUE;

                        node_risk_add(g, m.at_node, NODE_RISK_AMBUSH);

                        BS_LOG_INFO("ShipAI piracy: raiders ambushed contract %d at N%d (%.0f units of %s lost)%s%s -- node risk %.2f",

                                    victim, m.at_node, t.cargo_units, TRADE_GOOD_NAMES[t.cargo_good],

                                    escorted ? " -- fought past its escort" : "",

                                    patrolled ? " despite patrol presence" : "", node_risk_get(g, m.at_node));

                    } else if (victim >= 0 && escorted) {

                        BS_LOG_INFO("ShipAI piracy: convoy escort drove off raiders at N%d (contract %d protected)",

                                    m.at_node, victim);

                    } else {

                        BS_LOG_INFO("ShipAI piracy: raid at N%d found no prey%s",

                                    m.at_node, patrolled ? " (patrol presence)" : "");

                    }

                }

                m.active = FALSE; m.stage = MISSION_STAGE_COOLDOWN;

                m.ship_slot = -1; m.local_ready = FALSE;

                return;

            }

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

    // Phase 4: throttled military logistics survey (reinforcement columns + patrol circuits).

    g.military_tick_hours += sim_dt_hours;

    if (g.military_tick_hours >= MILITARY_TICK_HOURS) {

        g.military_tick_hours = 0.0f;

        ship_missions_military_tick(s);

    }

    // Phase 6: throttled economic settlement (trade income -> power, war chests, hulls, risk decay).

    g.economy_tick_hours += sim_dt_hours;

    if (g.economy_tick_hours >= ECONOMY_TICK_HOURS) {

        g.economy_tick_hours = 0.0f;

        ship_missions_economy_tick(s);

    }

    // The galaxy node the player currently occupies (current_system is a cache SLOT, not a node).

    i32 node_here = (g.current_system >= 0 && g.current_system < g.system_count)

                        ? g.cache_node[g.current_system] : -1;

    for (i32 i = 0; i < g.mission_count; ++i) {

        ShipMission& m = g.missions[i];

        if (!m.active) {

            // Cooldown: the origin station issues a fresh contract once the pause elapses.

            if (m.stage == MISSION_STAGE_COOLDOWN && m.station_id >= 0) {

                m.respawn_hours -= sim_dt_hours;

                if (m.respawn_hours <= 0.0f) {

                    // Phase 6: a contract whose ship was destroyed cannot sail again on paper alone
                    // -- the owning civ's shipyard has to hand over a replacement hull first. While
                    // the slips are empty the route simply goes unserved, so sustained raiding
                    // measurably thins a civ's active fleet.
                    b8 hull_ok = !m.awaiting_hull || civ_take_hull(g, m.owner);

                    if (!hull_ok) {

                        m.respawn_hours = SHIPYARD_WAIT_HOURS;

                    } else if (!mission_issue_contract(s, m)) {

                        if (m.awaiting_hull) civ_return_hull(g, m.owner);   // no route: keep the hull

                        m.respawn_hours = STATION_CONTRACT_COOLDOWN;        // retry later

                    } else {

                        if (m.awaiting_hull)

                            BS_LOG_INFO("ShipAI shipyard: civ %d commissioned a replacement hull for N%d/%d (%d left on the slips)",

                                        (i32)m.owner, station_id_node(m.station_id), station_id_index(m.station_id),

                                        (m.owner >= 0 && m.owner < g.civ_count) ? (i32)g.civs[m.owner].hull_pool : 0);

                        m.awaiting_hull = FALSE;

                    }

                }

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

    // Phase 4: a destroyed military column is strength lost forever (reinforce strength was

    // debited at launch and is never credited). The slot is reusable by the next survey.

    if (m.objective == OBJ_REINFORCE || m.objective == OBJ_PATROL || m.objective == OBJ_RAID) {

        BS_LOG_INFO("ShipAI military: %s destroyed en route (owner %d, strength %.0f lost)",

                    (m.objective == OBJ_REINFORCE) ? "reinforcement column"

                    : (m.objective == OBJ_PATROL)  ? "patrol" : "raid party",

                    (i32)m.owner, m.cargo_units);

        m.active = FALSE; m.stage = MISSION_STAGE_COOLDOWN;

        m.ship_slot = -1; m.local_ready = FALSE;

        return;

    }

    // Contract failed: the origin station re-issues after the standard cooldown -- but only once

    // its civ's shipyard has replaced the lost hull (Phase 6).

    m.active        = FALSE;

    m.stage         = MISSION_STAGE_COOLDOWN;

    m.respawn_hours = STATION_CONTRACT_COOLDOWN;

    m.ship_slot     = -1;

    m.local_ready   = FALSE;

    m.awaiting_hull = TRUE;

    BS_LOG_INFO("ShipAI trade: mission %d destroyed in-system (contract failed; awaiting a replacement hull)", mission_id);

}



