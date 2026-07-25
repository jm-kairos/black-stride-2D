# Trade Economy System

Black Stride's economy is a simulated AI-driven trade network where stations issue contracts, trader ships haul goods between star systems, and markets respond to supply and demand.

## Trade Goods

Three goods with distinct economic roles:

| Good | Base Price | Volatility | Role |
|------|-----------|------------|------|
| FOOD | 10 cr | 0.1 | Sustains populations; high supply in fertile systems |
| ORE  | 15 cr | 0.3 | Raw materials; high supply in barren systems |
| TECH | 40 cr | 0.6 | Manufactured goods; requires a living civilization |

Defined in `state/game_state.h:629` (`enum TradeGood : u8 { GOOD_FOOD=0, GOOD_ORE, GOOD_TECH, GOOD_COUNT };`). Base prices and volatility live in `station_market.cpp:15` and `:22`.

## Station Markets

### Baseline Market

Each station has a deterministic baseline market computed from the node seed (`station_market_baseline` in `station_market.cpp:56`). Baseline stock is biased by:
- **Habitability**: fertile systems overproduce FOOD, barren systems overproduce ORE
- **Civilization status**: alive civs produce TECH; uninhabited systems don't
- **Demand multiplier**: inverse of production bias — producers pay less, importers pay more

Stock range: 10–400 units per good. This creates natural price spreads between stations even at baseline.

### Pricing Model

One rule (`station_market.cpp:38-44`) with a per-good volatility modifier (`station_market.cpp:82-83`):

```
price_mod = 1 + PRICE_VOLATILITY[good] * (2 * random01 - 1)
price     = BASE_PRICE[good] * demand_mul * clamp(2 - stock/base_stock, 0.5, 2.0) * price_mod
```

- Scarce stock (ratio < 1): price rises up to 2x base
- Glutted stock (ratio > 1): price falls down to 0.5x base
- `demand_mul` = `1/production_bias`, so producers sell cheap, importers pay premium
- `price_mod` is baked into the baseline price and carried forward in `station_market_get`, giving each station a small random price variance even at baseline

### Live Market & Deltas

Live market = baseline + delta. Deltas are mutable stock offsets stored in a bounded pool (`StationMarketDelta`, 256 slots) in `GalaxyState`. When a trader loads cargo at origin, stock goes down (delta negative). When cargo is delivered, stock goes up (delta positive). Stations without a delta entry are at baseline.

### Market Decay

Deltas decay back toward baseline at 2.0 units/hour (`MARKET_DECAY_PER_HOUR`). This means trade disruptions are temporary — prices recover over time. Decay runs every frame via `station_markets_decay`.

## Trade Tiers

Contracts are weighted by distance and political relationship (`ship_mission.cpp:86-90` for the constants, `:302-309` for the logic):

| Tier | Description | Weight | Hops |
|------|------------|--------|------|
| 0 | Intra-system (same node, different station) | 1000x | 0 |
| 1 | Intra-civ (same owner, nearby) | 100x | 1-2 |
| 1b | Intra-civ (same owner, distant) | 10x | 3+ |
| 2 | Nearby uninhabited/foreign | 10x | 1-2 |
| 3 | Inter-civ global (rare long-distance) | 1x | 3+ |

This ensures most trade is local, with rare long-distance hauls creating economic diversity.

## Contract Lifecycle

### Issuance

Each mission-hub station (first `MISSION_HUBS_PER_SYSTEM=3` stations per system) issues one contract. Only habited systems issue contracts; uninhabited stations are receive-only destinations.

`mission_issue_contract` (`ship_mission.cpp:477`):
1. **Pick cargo**: origin's biggest surplus good (highest stock/baseline ratio); cargo units are `min(100, 0.5 * stock)` (`ship_mission.cpp:511`)
2. **Pick market**: scored sampling of up to 3-hop neighbors plus 1–2 random far nodes; `score = trade_value * random(0.5..1.5) * tier_weight * (dest_price / origin_price)`, with tier-ordered fallbacks (`ship_mission.cpp:312-464`)
3. **Settle forward reward**: `cargo_units × (dest_price - origin_price)`, min haulage fee
4. **Evaluate return cargo**: scan destination for surplus goods with positive spread back to origin
5. **Settle return reward**: `return_units × (origin_price - dest_price)`, min haulage fee; return units use the same `min(100, 0.5 * stock)` cap (`ship_mission.cpp:621`)
6. **Cache station positions**, route first chunk, enter ACQUIRE stage

### Mission Stages

The `MissionStage` enum (`game_state.h:1023-1042`) is ordered:

```
ORIGIN_DOCK → ACQUIRE → TO_JUMP → JUMP → CROSS → FINAL_APPROACH → MARKET_DOCK → COOLDOWN
```

At runtime a freshly-issued contract enters `ACQUIRE` first, then `ORIGIN_DOCK` before leaving the system.

| Stage | Description | Speed |
|-------|------------|-------|
| ORIGIN_DOCK | Loading cargo (18h dwell) | — |
| ACQUIRE | Spawn point → origin station | ai_speed_in_system |
| TO_JUMP | Origin station → exit jump-point | ai_speed_in_system |
| JUMP | Between systems | ai_speed_jump |
| CROSS | Intermediate system traversal | ai_speed_in_system |
| FINAL_APPROACH | Jump-point → destination station | ai_speed_in_system |
| MARKET_DOCK | Unloading cargo (18h dwell) | — |
| COOLDOWN | 60h before re-issue | — |

Tier-0 (intra-system) contracts skip TO_JUMP/JUMP/CROSS — go directly to FINAL_APPROACH.

### Bidirectional Return Leg

After MARKET_DOCK delivery, if a profitable return cargo was settled at issue time:

1. **Deplete destination** of return cargo (`station_market_apply` negative)
2. **Re-path** back to home node (`mission_repath`)
3. **Set `return_leg = TRUE`** — modifies JUMP and FINAL_APPROACH behavior:
   - JUMP arrival targets `station_pos` (origin) instead of `dest_station_pos`
   - FINAL_APPROACH transitions to `ORIGIN_DOCK` instead of `MARKET_DOCK`
4. **ORIGIN_DOCK return handler**: unloads return cargo at origin, credits destination station revenue, enters cooldown

If no profitable return cargo exists, the contract goes straight to cooldown after export delivery.

## Station Revenue

Cumulative credits earned by each station from trade activity. Tracked in a bounded pool (`StationRevenue`, 256 slots) in `GalaxyState`. No decay — revenue is permanent. When the pool is full, the station with the lowest recorded revenue is evicted (`station_market.cpp:150-169`).

**Export delivery**: origin station earns `cargo_units × origin_price` (selling surplus at local price)

**Return delivery**: destination station earns `return_units × dest_price` (selling return surplus at their price)

API: `station_revenue_add` / `station_revenue_get` in `station_market.h/.cpp`.

Displayed in station inspector:
- **Market tab**: `Revenue: X cr` line (all stations)
- **Contracts tab**: `Station revenue: X credits` header (stations with Contracts room)

## Key Data Structures

### ShipMission (`game_state.h:1047`)

Core contract state: owner, objective, route, cargo manifest, reward, station anchors, stage machine, return leg fields.

### StationMarketDelta (`game_state.h:649`)

Per-station stock offsets: `station_id` + `stock_delta[GOOD_COUNT]`. Bounded pool of 256, free slots marked `station_id = -1`.

### StationRevenue (`game_state.h:665`)

Per-station cumulative credits: `station_id` + `total_credits`. Bounded pool of 256, free slots marked `station_id = -1`.

## Key Files

| File | Role |
|------|------|
| `sim/station_market.h/.cpp` | Market model, pricing, deltas, decay, revenue API |
| `sim/ship_mission.cpp` | Contract lifecycle, mission stage machine, cargo selection |
| `sim/galaxy_map.cpp` | Pool initialization, station layout, galaxy graph |
| `state/game_state.h` | All data structures (ShipMission, StationMarketDelta, StationRevenue) |
| `game.cpp` | Station inspector UI (Dock/Market/Contracts tabs) |

## Tuning Constants

| Constant | Value | Location |
|----------|-------|----------|
| `TRADE_DWELL_HOURS` | 18.0h | `ship_mission.cpp:61` |
| `STATION_CONTRACT_COOLDOWN` | 60.0h | `ship_mission.cpp:69` |
| `MARKET_DECAY_PER_HOUR` | 2.0 units | `station_market.cpp:18` |
| `MISSION_HUBS_PER_SYSTEM` | 3 | `game_state.h:617` |
| `MISSION_MAX` | 8192 | `game_state.h:987` |
| `STATION_MARKET_DELTA_MAX` | 256 | `game_state.h:657` |
| `STATION_REVENUE_MAX` | 256 | `game_state.h:673` |
| `MISSION_ROUTE_MAX` | 32 hops | `game_state.h:985` |
| `ai_speed_in_system` | 50,000 u/s | `galaxy_map.cpp:526` |
| `ai_speed_jump` | 1,000,000 u/s | `galaxy_map.cpp:527` |

Note: `ai_speed_in_system` and `ai_speed_jump` are editable `GalaxyState` fields initialized at the listed locations, not `const` compile-time values.
