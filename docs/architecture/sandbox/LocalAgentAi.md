# LocalAgentAi

**Responsibility:** Owns the transient NPC agents that exist only in the player's current system
— loading their hull and weapon templates, materialising and retiring the population, the
behaviour state machine, perception, combat wings, the archetype behaviours (patrol, trader,
miner), and the bridge that turns an arriving macro traveller into a live hull. It explicitly
does not own the macro tier (MacroMissions owns persistent galaxy-wide travellers), does not own
combat resolution (CombatArena), and does not own how many agents a system should have — that
is read from GalaxyHistory's garrison layer.

> **Flagged as a god object.** `sim/ai_ship.cpp` is 2524 lines and sits in three cycles. It is
> documented as one subsystem by decision rather than split speculatively; see
> `sandbox-subsystems.md` §Settled calls.

**Public interface:** `sandbox/source/sim/ai_ship.h` — `enum ShipArchetype` (7 + count),
`enum AiState` (10 states), `struct AiProfile` (14 tunables), `ai_profile`, `ai_ships_init`,
`ai_ships_update`, `ai_ships_register_combat`, `ai_ship_damage`,
`ai_ships_debug_spawn_strike`. `NpcShip` itself lives in `state/game_state.h`.
Used from outside by 6 subsystems.

**Depends on:** GalaxyHistory, MacroMissions, Discovery, Economy, GalaxyRuntime, FleetControl
(`steering.h` — its only external consumer), ShipCombatModel, GameStateModel; engine
`core/logger.h`, `defines.h`.
**Depended on by:** CombatArena, Discovery, GalaxyHistory, InWorldOverlays, MacroMissions,
FrameOrchestrator.

**Key invariants:**
- **`ai_ships_init` is the release path for the shared per-archetype `Weapon` instances**, and
  `ai_ship.h` states why: the engine's `Game` struct exposes no shutdown hook, so idempotent
  re-init *is* how a previous set gets freed. Calling it on a new galaxy or restart must
  therefore stay safe to repeat. A missing engine feature dictates the module's lifetime design.
- **The NPC combat-entity window must stay packed after the player window.** Agents append at
  `npc_combat_base`, which CombatArena recomputes in `combat_arena_rebuild_player_entities`.
  Maintained jointly across two subsystems with no assertion.
- **Kill attribution is faction-sensitive:** only `FACTION_PLAYER` kills raid the victim's
  civilization and cost reputation; NPC-vs-NPC kills merely shrink the garrison or retire the
  mission. Stated in `ai_ship.h` and implemented in `ai_ship_damage`.
- **One `Weapon` instance is shared per archetype**, so its cooldown is shared by every agent of
  that type. A consequence of the ownership design, not documented as intended.
- Agents are transient by contract — they exist only in the player's current system, which is
  what distinguishes this tier from MacroMissions (`sim/ship_mission.h` draws the line).
- `ai_ship_damage` calls back into `ship_mission_notify_destroyed`, so destroying a materialised
  traveller permanently retires its macro mission.

**Extension points:** **The intended extension is a data row.** `ai_ship.h` states the design
thesis: one FSM for every ship type, differentiated only by `AiProfile` data and an
`enabled_states` bitmask, "so new ship kinds are added as data rows — no new AI code". Adding an
archetype means a value in `ShipArchetype`, a profile row returned by `ai_profile`, a ship
registry card id in `archetype_hull_id` (the card itself lives in `assets/ships/ships.list`;
ids missing from the registry fall back to the shared `raider` hull), and a weapon id in
`archetype_weapon_id`. A card that authors a `hull` line overrides the archetype's HP literal
at spawn (`hull_authored`); un-authored cards keep the literals, so per-hull toughness is an
opt-in data edit. A genuinely new *behaviour*
needs an `AiState` value plus a tick function following `ai_trader_tick` / `ai_miner_tick`, and
inclusion in the archetype's `enabled_states` mask.

**Known limitations / tech debt:**
- **It is a god object.** 2524 lines covering population management, materialisation, an FSM,
  perception, combat wings, trading, mining, delivery, and the macro→local handoff.
- **Three cycles**, one edge each way: with GalaxyHistory (`ai_ship.cpp` → `galaxy_history.h`,
  `galaxy_history.cpp` → `ai_ship.h`), with MacroMissions, and with Discovery. All three are
  legitimate collaborations; the problem is the file's size, not the boundaries.
- **The design thesis is only partly realised.** Several `ShipArchetype` values and `AiState`
  states are annotated "(Phase C)" — declared but unimplemented — so the enums describe an
  intended design larger than the code. `AiProfile` likewise mixes live tunables with fields
  annotated for future phases.
- **A fourth independent splitmix64** (`sm64`) lives here, described in-file as
  "deterministic-ish" — the qualifier acknowledging that agent spawning is not fully
  reproducible, unlike the rest of worldgen.
- Population density is an emergent output of the deep-time history simulation (read from the
  garrison layer), which makes agent counts hard to reason about in isolation.
- Miners write directly into station markets via `station_market_apply`, closing an economy loop
  from ambient AI — powerful, but it means a rendering-invisible background system mutates
  tradeable state.
- `ai_ships_debug_spawn_strike` is a test harness exported in the production header, wired to a
  keybinding in `game.cpp`.
- Together with `sim/ship_mission.cpp` (2735 lines) this is ~5,200 lines across the two AI
  tiers. *Inferred:* they would be better decomposed together than separately, since the seams
  between them (`ai_ships_sync_missions`, `ship_mission_notify_destroyed`) are exactly where the
  responsibilities blur — but the code states no such intent.

**Source paths:** `sandbox/source/sim/ai_ship.{cpp,h}`

**Last verified:** 2026-08-13, working tree on `game` (hull templates now instantiate from the
ship registry: `archetype_hull_path` became `archetype_hull_id`, `ai_ships_init` builds
`npc_template`/`npc_hulls` via `ship_instantiate` and no longer resolves textures per template —
it copies the def's handles, which is why `ship_registry_resolve_textures` must run before it
in `game_init`. Previously 2026-08-07, commit `812680c`.)
