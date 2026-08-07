# Economy

**Responsibility:** Owns the station economy — the trade-good catalogue, each station's
deterministic room list and baseline market, the bounded pool of stock deltas that trade
produces, price formation, decay back to baseline, and cumulative station revenue. It explicitly
does not own stations' *existence* or position (GalaxyRuntime owns the deterministic station
layout and identity), does not own who trades (MacroMissions' dock hooks and LocalAgentAi's
miners are the writers), and does not own any UI — the station inspector lives in DevPanels.

**Public interface:** `sandbox/source/sim/station_market.h` — `struct MarketGood`;
`TRADE_GOOD_NAMES`, `TRADE_CATEGORY_NAMES`; `trade_good_category`, `trade_good_base_price`;
`station_specialization`, `station_rooms`; `station_market_baseline`, `station_market_get`,
`station_market_apply`, `station_markets_decay`; `station_revenue_add`, `station_revenue_get`.
`TradeGood`, `TradeCategory`, `StationRoomKind` and `StationMarketDelta` live in
`state/game_state.h`. Used from outside by 3 subsystems.

**Depends on:** GalaxyRuntime (only for the `station_id_*` packing helpers), GameStateModel;
engine `defines.h`.
**Depended on by:** LocalAgentAi, MacroMissions, FrameOrchestrator.

**Key invariants:**
- **Rooms and baseline markets are pure functions of the station id.** `station_market.h` states
  the philosophy — "same philosophy as the station layout core in `galaxy_map.cpp`" — and that
  **nothing is stored per station**. `station_market_baseline` must therefore return identical
  values on every call for a given id; it is described in the header as "Pure function —
  identical every call."
- **The only mutable state is `GalaxyState::market_deltas`**, a bounded pool of per-station
  stock offsets. Everything else is recomputed.
- **Price follows one rule:** `price = base_price * demand_mul * clamp(2 - stock/base_stock,
  0.5, 2.0)`. Scarcity and glut map to price through a single clamped ratio, documented in the
  implementation.
- **Deltas decay toward baseline at a fixed 2 units per in-game hour**
  (`MARKET_DECAY_PER_HOUR`), tying the economy's memory to the shared sim clock. Slots that
  reach baseline are freed.
- **Baselines are biased by the node's abundance signals** (habitability, biosphere, ore and
  volatile richness, civ industry) and boosted for the station's specialised category, so a
  fertile system naturally hosts agricultural hubs. That coupling runs from GalaxyGeneration's
  per-node summary through to price.
- `station_market_get` is baseline plus the delta entry if one exists, with stock clamped ≥ 0;
  stations without an entry are exactly at baseline.

**Extension points:** **A new trade good** is a value in `TradeGood`
(`state/game_state.h`), a `TRADE_GOOD_NAMES` entry, and a `GOOD_INFO` row giving its category,
base price and volatility — the three are positional and must stay aligned. **A new category**
is a `TradeCategory` value, a `TRADE_CATEGORY_NAMES` entry, and a branch in
`station_specialization`'s abundance-weighted roll. **A new room kind** is a
`StationRoomKind` value plus a rule in `station_rooms`; today every station has DOCK + MARKET and
mission-hub stations (the first `MISSION_HUBS_PER_SYSTEM` of the layout) also have CONTRACTS.
A new market *writer* calls `station_market_apply`, which finds or allocates the station's delta
slot.

**Known limitations / tech debt:**
- **The delta pool evicts the closest-to-baseline entry when full**, so in a busy galaxy the
  least-displaced market silently loses its history. `STATION_MARKET_DELTA_MAX` is 256 against
  a galaxy of ~10,000 nodes.
- **Revenue entries share the same pool but never decay**, so two different lifetimes coexist in
  one structure with one capacity.
- The whole economic model is one table (`GOOD_INFO`) of ten goods in four categories. That is
  the entire economic content — there is no production, no consumption model, no transport cost,
  and no price memory beyond the decaying delta.
- `station_specialization` and `station_market_baseline` both re-derive from the node seed on
  every call. Cheap per call, but the market UI and the AI's routing both call them repeatedly
  per frame with no memoisation.
- Ten goods are declared and the AI writers use a subset — miners pick via `miner_pick_good` and
  traders via `planet_export_good` (both in `sim/ai_ship.cpp`), so which goods actually move is
  decided elsewhere.
- The header names `ship_mission.cpp`'s dock hooks as the caller of `station_market_apply`,
  which is the only record of who mutates the economy; `sim/ai_ship.cpp`'s miner delivery is a
  second writer not mentioned there.
- Prices are read by the station inspector and by AI routing, but nothing exposes a trade action
  to the player — the economy is currently observable and AI-driven only. *Inferred:* that
  player trading is intended (`WeaponDef` carries `price`/`tier` marked "market-forward: unused
  v1"), but this subsystem states no such plan.

**Source paths:** `sandbox/source/sim/station_market.{cpp,h}`

**Last verified:** 2026-08-07, commit `812680c`
