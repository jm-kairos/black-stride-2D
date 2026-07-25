#pragma once
#include <defines.h>

struct game_state;

// =====================================================================================
// station_market.h — minimal interstellar economy (KISS).
//
// Stations are room-based: every station has a Dock and a Market room; mission-hub stations also
// have a Contracts room (the thing that already issues trade contracts). Rooms and the BASELINE
// market are pure deterministic functions of the station id (same philosophy as the station
// layout core in galaxy_map.cpp) — nothing is stored per station.
//
// The only mutable state is GalaxyState::market_deltas: a bounded pool of per-station stock
// offsets written by trader docks (load = stock down, deliver = stock up) that decay back to
// baseline over in-game hours. Price follows one rule: scarce -> expensive, glutted -> cheap.
// =====================================================================================

// Snapshot of one good at a station: current stock (baseline + delta), the deterministic baseline
// stock, and the resulting unit price.
struct MarketGood {
    f32 stock;
    f32 base_stock;
    f32 price;
};

// Display names indexable by TradeGood (GOOD_COUNT entries).
extern const char* const TRADE_GOOD_NAMES[3];

// Room list of a station (StationRoomKind values into out[]). Every station has DOCK + MARKET;
// mission-hub stations (first MISSION_HUBS_PER_SYSTEM of the layout) also have CONTRACTS.
// Returns the room count.
i32 station_rooms(const game_state* s, i32 station_id, u8* out, i32 max_out);

// Deterministic baseline market of a station (out must hold GOOD_COUNT entries): base stock
// (50..250 before bias, 10..400 after bias/clamp) per good from the node seed, biased by
// habitability (food vs ore) and whether an alive civilization holds the system (tech).
// Pure function — identical every call.
void station_market_baseline(const game_state* s, i32 station_id, MarketGood* out);

// Live market: baseline plus this station's delta-pool entry (if any), stock clamped >= 0 and
// prices recomputed. Stations without a delta entry are exactly at baseline.
void station_market_get(const game_state* s, i32 station_id, MarketGood* out);

// Shift a station's stock of `good` by delta_units (negative = goods leave the station). Finds or
// allocates the station's delta-pool slot; when the pool is full the closest-to-baseline entry is
// evicted. Called by the mission dock hooks in ship_mission.cpp.
void station_market_apply(game_state* s, i32 station_id, i32 good, f32 delta_units);

// Decay every delta toward zero (back to baseline) by the fixed per-hour rate; frees slots that
// reach baseline. Call once per frame with elapsed in-game hours.
void station_markets_decay(game_state* s, f32 hours);

// Add cumulative trade revenue to a station (finds or allocates a pool slot). No decay.
void station_revenue_add(game_state* s, i32 station_id, f32 credits);

// Get cumulative trade revenue for a station (0 if no entry).
f32 station_revenue_get(const game_state* s, i32 station_id);
