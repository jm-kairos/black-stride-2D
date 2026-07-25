#include "sim/station_market.h"

#include "game.h"              // full game_state (GalaxyState, GalaxyNode, TradeGood, delta pool)
#include "sim/galaxy_map.h"    // station_id_node / station_id_index

#include <math.h>

using GalaxyState = game_state::GalaxyState;

const char* const TRADE_GOOD_NAMES[3] = { "FOOD", "ORE", "TECH" };

// ---- Tuning ---------------------------------------------------------------------------------
// Base unit price of each good at exactly-baseline stock. The whole supply/demand model is one
// rule: price = BASE_PRICE * clamp(2 - stock/base_stock, 0.5, 2.0).
static const f32 BASE_PRICE[GOOD_COUNT] = { 10.0f, 15.0f, 40.0f };

// Stock deltas decay back toward baseline at this many units per in-game hour.
static const f32 MARKET_DECAY_PER_HOUR = 2.0f;

// Per-good price volatility: higher-tier goods (TECH) have more price variance between
// stations than basic goods (FOOD). Creates non-zero price spreads even at baseline.
static const f32 PRICE_VOLATILITY[GOOD_COUNT] = { 0.1f, 0.3f, 0.6f };  // FOOD, ORE, TECH

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
    return BASE_PRICE[good] * demand_mul * mul;
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
    const GalaxyState& g = s->galaxy;
    i32 node  = (station_id >= 0) ? station_id_node(station_id) : -1;
    f32 hab   = 0.5f;
    b8  alive = FALSE;
    u64 seed  = (u64)(station_id + 1) * 0x9E3779B97F4A7C15ull;   // defensive: id out of range
    if (node >= 0 && node < g.node_count) {
        hab = (f32)g.nodes[node].best_habitability / 255.0f;
        i16 owner = g.node_owner ? g.node_owner[node] : (i16)-1;
        alive = (owner >= 0 && owner < g.civ_count && g.civs[owner].status == 0);
        seed  = g.nodes[node].seed ^ ((u64)(station_id + 1) * 0x100000001B3ull);
    }
    // Per-good production bias: fertile systems overflow with food, barren ones dig ore instead,
    // and tech needs a living civilization's industry.
    f32 bias[GOOD_COUNT];
    bias[GOOD_FOOD] = 0.5f + hab;
    bias[GOOD_ORE]  = 1.5f - hab;
    bias[GOOD_TECH] = alive ? 1.2f : 0.5f;
    for (i32 gd = 0; gd < GOOD_COUNT; ++gd) {
        f32 base = (50.0f + 200.0f * mix01(seed ^ ((u64)(gd + 1) * 0xC0FFEEull))) * bias[gd];
        if (base < 10.0f)  base = 10.0f;
        if (base > 400.0f) base = 400.0f;
        out[gd].base_stock = base;
        out[gd].stock      = base;
        f32 demand_mul     = 1.0f / bias[gd];   // producer pays less, importer pays more
        f32 vol            = PRICE_VOLATILITY[gd];
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
            f32 demand_mul = (BASE_PRICE[gd] > 0.0f) ? baseline_price / BASE_PRICE[gd] : 1.0f;
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
