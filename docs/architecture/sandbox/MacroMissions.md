# MacroMissions

**Responsibility:** Owns the macro traveller tier — persistent, player-independent agents that
walk the galaxy lane graph toward an objective. It owns their routing and multi-stage travel
machine, trade and military contract issuance, the civ economy settlement (trade income, fleet
caps, hull availability), and the per-node risk table. It explicitly does not own local agents
(LocalAgentAi owns transient `NpcShip`s; `sim/ship_mission.h` draws the line: macro travellers
exist everywhere at all times and move in in-game hours), does not own station markets
(Economy), and does not draw the map pips (GalaxyMapRendering, which calls
`ship_mission_position`).

**Public interface:** `sandbox/source/sim/ship_mission.h` — `ship_missions_seed`,
`ship_missions_update`, `ship_mission_node_risk`, `ship_mission_position`,
`ship_mission_notify_destroyed`. `ShipMission`, `MissionObjective` and `MissionStage` live in
`state/game_state.h`. Used from outside by 3 subsystems.

**Depends on:** GalaxyRuntime, GalaxyHistory, Economy, LocalAgentAi, GameStateModel; engine
`math/bs_hierpos.h`, `core/logger.h`, `defines.h`.
**Depended on by:** GalaxyMapRendering, LocalAgentAi, FrameOrchestrator.

**Key invariants:**
- **`ship_missions_seed` must run once at generation end**, after garrison seeding, when the
  node/lane graph and civ ownership are final — stated in the header and satisfied by
  `run_generation_stage` in `game.cpp`. It is a no-op if the mission pool is unallocated.
- **`ship_mission_position` is stage-dependent and must be used instead of reading the field.**
  During dock and cooldown stages the relevant station anchor is authoritative; otherwise the
  continuously-integrated `ShipMission::pos` is. The header says so; rendering and the
  local-agent handoff both go through it.
- **Time is in in-game hours, and `1 real second == 1 in-game hour at 1x`**, so `sim_dt_hours`
  doubles as seconds. That unit pun is what lets one clock drive both AI tiers; it is documented
  in the header and relied on by the speed fields.
- **The economic loop is closed:** trade → wealth → power → fleets. Trade income accrues to
  civs, `civ_afford` and `civ_fleet_cap` gate military spending, and `civ_take_hull` /
  `civ_return_hull` limit how many missions a civ can field. Breaking any link decouples the
  economy from military pressure.
- **Node risk is a feedback channel**: raider sorties and successful ambushes raise it, it decays
  each economy settlement, and both trade routing and the live AI read it — so interceptors get
  posted where lanes are actually bleeding.
- **`ship_mission_notify_destroyed` permanently retires a mission**, freeing its pool slot; it is
  a no-op for an out-of-range or already-inactive id. This is the writeback half of the
  macro↔local seam.
- `ship_missions_update` is a no-op when `sim_dt_hours <= 0`, which is how pause is honoured.

**Extension points:** **A new objective** is a value in `MissionObjective`, an issuance function
following `mission_issue_contract` (trade) or `mission_issue_military` (reinforce/patrol/raid),
and stage handling in `mission_travel_step`. **A new travel stage** is a `MissionStage` value
plus a branch in the stage machine; the existing chain (fly to station → load → fly to the jump
circle → jump → cross intermediate systems → dock at the market) is documented in the header.
**A new economic pressure** hooks into `ship_missions_economy_tick`, which is where trade income
settles and fleet caps are recomputed. Speeds are editor-tunable via
`GalaxyState::ai_speed_in_system` / `ai_speed_jump` rather than constants here.

**Known limitations / tech debt:**
- **2735 lines — the second-largest file in the sandbox** after `state/game_state.h`, and the
  largest simulation file. `mission_travel_step` alone is roughly 600 lines.
- **In a 1/1 cycle with LocalAgentAi.** The two AI tiers total ~5,200 lines and their seams
  (`ai_ships_sync_missions`, `ship_mission_notify_destroyed`) are exactly where the
  responsibilities blur. *Inferred:* they would be better decomposed together than separately;
  the code states no such intent.
- **`mission_stall_check` exists because the state machine can deadlock** — an explicit watchdog
  on missions that wait too long at a target. That a watchdog was needed is the finding.
- **A fifth independent splitmix64** lives here for its own rolls.
- `MISSION_MAX` is 8192 and the pool is **heap-allocated**, unlike nearly every other capacity in
  the sandbox, which is a fixed inline array — so mission storage has a different lifetime model
  from everything around it.
- The module writes into station markets through its dock hooks, making trader arrivals the
  source of the delta pool Economy decays — a cross-subsystem write with no interface beyond
  `station_market_apply`.
- It reads and writes civ state (credits, fleet strength, hull counts) directly on
  `Civilization` records owned by GalaxyHistory, rather than through that subsystem's API.
- The header describes five entry points; the hidden simulation behind them is by far the
  largest ratio of implementation to interface in the project.

**Source paths:** `sandbox/source/sim/ship_mission.{cpp,h}`

**Last verified:** 2026-08-07, commit `812680c`
