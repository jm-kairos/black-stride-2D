#include "sim/station_market.h"

#include "game.h"              // full game_state (GalaxyState, GalaxyNode, TradeGood, delta pool)
#include "sim/galaxy_map.h"    // station_id_node / station_id_index

#include <math.h>

using GalaxyState = game_state::GalaxyState;

const char* const TRADE_GOOD_NAMES[GOOD_COUNT] = {
    "Grain", "Organics", "Iron Ore", "Rare Metals", "Water Ice",
    "Hydrogen", "Alloys", "Electronics", "Medicine", "Luxuries"
};

const char* const TRADE_CATEGORY_NAMES[CAT_COUNT] = {
    "Agriculture", "Minerals", "Volatiles", "Industrial"
};

// ---- Tuning ---------------------------------------------------------------------------------
// Static per-good metadata: category, base unit price at exactly-baseline stock, and price
// volatility (variance between stations; refined/luxury goods swing more than raw staples).
// The whole supply/demand model is still one rule:
// price = base_price * demand_mul * clamp(2 - stock/base_stock, 0.5, 2.0).
struct TradeGoodInfo {
    u8  category;     // TradeCategory
    f32 base_price;   // credits per unit at baseline
    f32 volatility;   // 0..1 baseline price variance between stations
};

static const TradeGoodInfo GOOD_INFO[GOOD_COUNT] = {
    { CAT_AGRICULTURE,  8.0f, 0.10f },  // GOOD_GRAIN
    { CAT_AGRICULTURE, 14.0f, 0.20f },  // GOOD_ORGANICS
    { CAT_MINERALS,    12.0f, 0.15f },  // GOOD_IRON_ORE
    { CAT_MINERALS,    45.0f, 0.40f },  // GOOD_RARE_METALS
    { CAT_VOLATILES,    6.0f, 0.10f },  // GOOD_WATER_ICE
    { CAT_VOLATILES,   18.0f, 0.25f },  // GOOD_HYDROGEN_FUEL
    { CAT_INDUSTRIAL,  30.0f, 0.30f },  // GOOD_ALLOYS
    { CAT_INDUSTRIAL,  60.0f, 0.50f },  // GOOD_ELECTRONICS
    { CAT_INDUSTRIAL,  75.0f, 0.45f },  // GOOD_MEDICINE
    { CAT_INDUSTRIAL,  90.0f, 0.60f },  // GOOD_LUXURIES
};

// Stock deltas decay back toward baseline at this many units per in-game hour.
static const f32 MARKET_DECAY_PER_HOUR = 2.0f;

// Baseline stock multiplier for goods in the station's specialized category: specialists
// overproduce their niche (cheap locally, profitable to export).
static const f32 SPECIALIZATION_STOCK_MUL = 1.6f;

// ---- Node economic signals -------------------------------------------------------------------
// The five 0..1 supply signals a node exposes to the market model. All are galaxy-generation
// summaries (GalaxyNode) or civ state — nothing materialises.
enum { SIG_HAB = 0, SIG_BIO, SIG_MET, SIG_VOL, SIG_IND, SIG_COUNT };

// Per-good supply weights over the node signals; supply strength = dot(weights, signals).
static const f32 GOOD_SUPPLY_W[GOOD_COUNT][SIG_COUNT] = {
    // hab    bio    met    vol    ind
    { 0.7f,  0.3f,  0.0f,  0.0f,  0.0f },  // GOOD_GRAIN         farms need habitable worlds
    { 0.3f,  0.7f,  0.0f,  0.0f,  0.0f },  // GOOD_ORGANICS      biomass needs a biosphere
    { 0.0f,  0.0f,  1.0f,  0.0f,  0.0f },  // GOOD_IRON_ORE      pure extraction
    { 0.0f,  0.0f,  0.8f,  0.0f,  0.2f },  // GOOD_RARE_METALS   extraction + some refining
    { 0.2f,  0.0f,  0.0f,  0.8f,  0.0f },  // GOOD_WATER_ICE     ice mining (wet worlds help)
    { 0.0f,  0.0f,  0.0f,  0.7f,  0.3f },  // GOOD_HYDROGEN_FUEL volatiles + refinery industry
    { 0.0f,  0.0f,  0.4f,  0.0f,  0.6f },  // GOOD_ALLOYS        ore fed through industry
    { 0.0f,  0.0f,  0.2f,  0.0f,  0.8f },  // GOOD_ELECTRONICS   high industry
    { 0.2f,  0.2f,  0.0f,  0.0f,  0.6f },  // GOOD_MEDICINE      industry + organic feedstock
    { 0.1f,  0.1f,  0.0f,  0.0f,  0.8f },  // GOOD_LUXURIES      wealthy industry
};

// Per-category supply weights (specialization roll): what makes a node a natural host for
// each kind of specialist station.
static const f32 CAT_SUPPLY_W[CAT_COUNT][SIG_COUNT] = {
    // hab    bio    met    vol    ind
    { 0.5f,  0.5f,  0.0f,  0.0f,  0.0f },  // CAT_AGRICULTURE
    { 0.0f,  0.0f,  0.9f,  0.0f,  0.1f },  // CAT_MINERALS
    { 0.1f,  0.0f,  0.0f,  0.8f,  0.1f },  // CAT_VOLATILES
    { 0.1f,  0.1f,  0.1f,  0.0f,  0.7f },  // CAT_INDUSTRIAL
};

// splitmix64 finalizer — independent deterministic stream (same family as the station layout
// PRNG in galaxy_map.cpp; never perturbs other generation).
static inline u64 market_mix(u64 x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
static inline f32 mix01(u64 x) { return (f32)(market_mix(x) & 0xFFFFFF) / (f32)0xFFFFFF; }

// The one price rule: scarce -> expensive (up to 2x base), glutted -> cheap (down to 0.5x base).
// demand_mul scales the base price per-station: producers (high supply) have low demand (pay
// less), importers (low supply) have high demand (pay more). This creates price spreads at
// baseline that make trade routes profitable.
static f32 good_price(i32 good, f32 stock, f32 base_stock, f32 demand_mul) {
    f32 ratio = (base_stock > 1.0f) ? stock / base_stock : 1.0f;
    f32 mul   = 2.0f - ratio;
    if (mul < 0.5f) mul = 0.5f;
    if (mul > 2.0f) mul = 2.0f;
    return GOOD_INFO[good].base_price * demand_mul * mul;
}

i32 trade_good_category(i32 good) {
    return (good >= 0 && good < GOOD_COUNT) ? (i32)GOOD_INFO[good].category : (i32)CAT_COUNT;
}

// Fill the node's five supply signals (all 0..1). Defensive defaults for out-of-range nodes
// match the old baseline behaviour (mid habitability, no industry).
static void node_signals(const game_state* s, i32 node, f32 sig[SIG_COUNT]) {
    const GalaxyState& g = s->galaxy;
    sig[SIG_HAB] = 0.5f; sig[SIG_BIO] = 0.0f; sig[SIG_MET] = 0.0f;
    sig[SIG_VOL] = 0.0f; sig[SIG_IND] = 0.0f;
    if (node < 0 || node >= g.node_count) return;
    const GalaxyNode& nd = g.nodes[node];
    sig[SIG_HAB] = (f32)nd.best_habitability / 255.0f;
    sig[SIG_BIO] = (f32)nd.biosphere         / 255.0f;
    sig[SIG_MET] = (f32)nd.res_metal         / 255.0f;
    sig[SIG_VOL] = (f32)nd.res_volatiles     / 255.0f;
    i16 owner = g.node_owner ? g.node_owner[node] : (i16)-1;
    if (owner >= 0 && owner < g.civ_count && g.civs[owner].status == 0) {
        f32 pn = g.civs[owner].power / 4.0f;   // civ power ~1+ -> 0..1 industry ramp
        if (pn > 1.0f) pn = 1.0f;
        sig[SIG_IND] = 0.5f + 0.5f * pn;
    }
}

// The deterministic per-station market seed (shared by baseline + specialization so both stay
// in lockstep for one station).
static u64 station_market_seed(const game_state* s, i32 station_id, i32 node) {
    const GalaxyState& g = s->galaxy;
    if (node >= 0 && node < g.node_count)
        return g.nodes[node].seed ^ ((u64)(station_id + 1) * 0x100000001B3ull);
    return (u64)(station_id + 1) * 0x9E3779B97F4A7C15ull;   // defensive: id out of range
}

i32 station_specialization(const game_state* s, i32 station_id) {
    i32 node = (station_id >= 0) ? station_id_node(station_id) : -1;
    f32 sig[SIG_COUNT];
    node_signals(s, node, sig);
    // Weighted roll: every category keeps a small floor so barren systems can still host the
    // odd off-profile specialist, but abundance dominates.
    f32 w[CAT_COUNT];
    f32 total = 0.0f;
    for (i32 c = 0; c < CAT_COUNT; ++c) {
        f32 t = 0.05f;
        for (i32 k = 0; k < SIG_COUNT; ++k) t += CAT_SUPPLY_W[c][k] * sig[k];
        w[c] = t;
        total += t;
    }
    f32 r = mix01(station_market_seed(s, station_id, node) ^ 0x5EC1A17ull) * total;
    for (i32 c = 0; c < CAT_COUNT; ++c) {
        r -= w[c];
        if (r <= 0.0f) return c;
    }
    return CAT_COUNT - 1;
}

i32 station_rooms(const game_state* s, i32 station_id, u8* out, i32 max_out) {
    i32 n = 0;
    if (n < max_out) out[n++] = (u8)ROOM_DOCK;
    if (n < max_out) out[n++] = (u8)ROOM_MARKET;

    // CONTRACTS room is only exposed for live mission hubs in habited systems.
    // Uninhabited stations may receive contracts but never originate them.
    i32 node  = (station_id >= 0) ? station_id_node(station_id) : -1;
    i16 owner = (node >= 0 && node < s->galaxy.node_count && s->galaxy.node_owner)
                    ? s->galaxy.node_owner[node] : (i16)-1;
    b8  alive = (owner >= 0 && owner < s->galaxy.civ_count && s->galaxy.civs[owner].status == 0);
    if (station_id >= 0 && alive && station_id_index(station_id) < MISSION_HUBS_PER_SYSTEM && n < max_out)
        out[n++] = (u8)ROOM_CONTRACTS;
    return n;
}

void station_market_baseline(const game_state* s, i32 station_id, MarketGood* out) {
    i32 node = (station_id >= 0) ? station_id_node(station_id) : -1;
    u64 seed = station_market_seed(s, station_id, node);
    f32 sig[SIG_COUNT];
    node_signals(s, node, sig);
    i32 spec = station_specialization(s, station_id);
    for (i32 gd = 0; gd < GOOD_COUNT; ++gd) {
        // Production bias: the node's supply strength for this good, boosted when the station
        // specializes in its category. Range comparable to the old 0.5..1.5 hand-tuned biases.
        f32 t = 0.0f;
        for (i32 k = 0; k < SIG_COUNT; ++k) t += GOOD_SUPPLY_W[gd][k] * sig[k];
        f32 bias = 0.4f + 1.2f * t;
        if ((i32)GOOD_INFO[gd].category == spec) bias *= SPECIALIZATION_STOCK_MUL;
        if (bias < 0.25f) bias = 0.25f;
        if (bias > 2.5f)  bias = 2.5f;
        f32 base = (50.0f + 200.0f * mix01(seed ^ ((u64)(gd + 1) * 0xC0FFEEull))) * bias;
        if (base < 10.0f)  base = 10.0f;
        if (base > 400.0f) base = 400.0f;
        out[gd].base_stock = base;
        out[gd].stock      = base;
        f32 demand_mul     = 1.0f / bias;   // producer pays less, importer pays more
        f32 vol            = GOOD_INFO[gd].volatility;
        f32 price_mod      = 1.0f + vol * (2.0f * mix01(seed ^ ((u64)(gd + 7) * 0xBEEFull)) - 1.0f);
        out[gd].price      = good_price(gd, base, base, demand_mul) * price_mod;
    }
}

void station_market_get(const game_state* s, i32 station_id, MarketGood* out) {
    station_market_baseline(s, station_id, out);
    if (station_id < 0) return;
    const GalaxyState& g = s->galaxy;
    for (i32 i = 0; i < STATION_MARKET_DELTA_MAX; ++i) {
        if (g.market_deltas[i].station_id != station_id) continue;
        for (i32 gd = 0; gd < GOOD_COUNT; ++gd) {
            f32 baseline_price = out[gd].price;  // captures demand_mul from baseline
            f32 demand_mul = (GOOD_INFO[gd].base_price > 0.0f) ? baseline_price / GOOD_INFO[gd].base_price : 1.0f;
            f32 st = out[gd].stock + g.market_deltas[i].stock_delta[gd];
            out[gd].stock = (st > 0.0f) ? st : 0.0f;
            out[gd].price = good_price(gd, out[gd].stock, out[gd].base_stock, demand_mul);
        }
        break;
    }
}

void station_market_apply(game_state* s, i32 station_id, i32 good, f32 delta_units) {
    if (station_id < 0 || good < 0 || good >= GOOD_COUNT || delta_units == 0.0f) return;
    GalaxyState& g = s->galaxy;
    StationMarketDelta* slot      = nullptr;
    StationMarketDelta* free_slot = nullptr;
    StationMarketDelta* evict     = nullptr;
    f32 evict_mag = 3.4e38f;
    for (i32 i = 0; i < STATION_MARKET_DELTA_MAX; ++i) {
        StationMarketDelta& d = g.market_deltas[i];
        if (d.station_id == station_id) { slot = &d; break; }
        if (d.station_id < 0) { if (!free_slot) free_slot = &d; continue; }
        f32 mag = 0.0f;
        for (i32 gd = 0; gd < GOOD_COUNT; ++gd) mag += fabsf(d.stock_delta[gd]);
        if (mag < evict_mag) { evict_mag = mag; evict = &d; }
    }
    if (!slot) {
        // Allocate: prefer a free slot, else evict the entry closest to baseline (it barely
        // mattered anyway) so the pool stays bounded.
        slot = free_slot ? free_slot : evict;
        if (!slot) return;
        slot->station_id = station_id;
        for (i32 gd = 0; gd < GOOD_COUNT; ++gd) slot->stock_delta[gd] = 0.0f;
    }
    slot->stock_delta[good] += delta_units;
}

void station_markets_decay(game_state* s, f32 hours) {
    if (hours <= 0.0f) return;
    f32 step = MARKET_DECAY_PER_HOUR * hours;
    GalaxyState& g = s->galaxy;
    for (i32 i = 0; i < STATION_MARKET_DELTA_MAX; ++i) {
        StationMarketDelta& d = g.market_deltas[i];
        if (d.station_id < 0) continue;
        b8 any = FALSE;
        for (i32 gd = 0; gd < GOOD_COUNT; ++gd) {
            f32 v = d.stock_delta[gd];
            if (v > step)       v -= step;
            else if (v < -step) v += step;
            else                v  = 0.0f;
            d.stock_delta[gd] = v;
            if (fabsf(v) > 0.01f) any = TRUE;
        }
        if (!any) d.station_id = -1;   // back at baseline: free the slot
    }
}

void station_revenue_add(game_state* s, i32 station_id, f32 credits) {
    if (station_id < 0 || credits <= 0.0f) return;
    GalaxyState& g = s->galaxy;
    StationRevenue* slot      = nullptr;
    StationRevenue* free_slot = nullptr;
    StationRevenue* evict     = nullptr;
    f32 evict_min = 3.4e38f;
    for (i32 i = 0; i < STATION_REVENUE_MAX; ++i) {
        StationRevenue& r = g.station_revenues[i];
        if (r.station_id == station_id) { slot = &r; break; }
        if (r.station_id < 0) { if (!free_slot) free_slot = &r; continue; }
        if (r.total_credits < evict_min) { evict_min = r.total_credits; evict = &r; }
    }
    if (!slot) {
        slot = free_slot ? free_slot : evict;
        if (!slot) return;
        slot->station_id    = station_id;
        slot->total_credits = 0.0f;
    }
    slot->total_credits += credits;
}

f32 station_revenue_get(const game_state* s, i32 station_id) {
    if (station_id < 0) return 0.0f;
    const GalaxyState& g = s->galaxy;
    for (i32 i = 0; i < STATION_REVENUE_MAX; ++i) {
        if (g.station_revenues[i].station_id == station_id)
            return g.station_revenues[i].total_credits;
    }
    return 0.0f;
}
