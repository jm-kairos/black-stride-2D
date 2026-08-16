# GalaxyRuntime

**Responsibility:** Owns the live galaxy — the hot cache of fully materialised star systems near
the camera, its per-frame reconciliation (materialise newcomers, evict the departed, track the
nearest system), orbital motion for cached systems, the map-entity list, lane routing, and
deterministic station identity and layout. It explicitly does not own generation
(GalaxyGeneration produces the node summaries and system content this caches), does not own
civilizations or ownership (GalaxyHistory), and does not draw the map (GalaxyMapRendering).

**Public interface:** `sandbox/source/sim/galaxy_map.h` — `GALAXY_MATERIALIZE_RADIUS`;
`galaxy_map_init`, `_worldgen`, `_finalize`, `_sync_entities`, `_update_orbits`;
`galaxy_materialize_update`, `galaxy_nearest_node`, `galaxy_ensure_materialized`;
`galaxy_route_find`, `galaxy_lane_length`; `station_id_make` / `_node` / `_index`,
`struct StationLayoutEntry`, `galaxy_node_station_layout`, `galaxy_station_pos_by_id`.
Used from outside by **8 subsystems** — the most-consumed simulation header.

**Depends on:** GalaxyGeneration, GalaxyHistory, GameStateModel; engine
`core/memory/bs_memory.h`, `core/logger.h`, `math/bs_hierpos.h`, `defines.h`.
**Depended on by:** CelestialParallax, Discovery, Economy, GalaxyHistory, GalaxyMapRendering,
LocalAgentAi, MacroMissions, FrameOrchestrator.

**Key invariants:**
- **`s->galaxy.current_system` is a cache *slot*, not a node index.** The cache holds
  `GALAXY_MAX_SYSTEMS` (64) entries while `node_count` reaches ~10,000. Every consumer that
  stores one must treat it as frame-scoped — `render/galaxy_map_render.h` states its returned
  slot "is only valid this frame" and tells callers to stash the system's `galaxy_center`
  instead. This is the single most important thing to get right about this subsystem.
- **`GALAXY_MATERIALIZE_RADIUS` (2.4e9) is deliberately smaller than the ~2e9 inter-system
  spacing**, so only a few non-overlapping neighbours ever draw full orbit detail at once — a
  rendering budget expressed as a simulation constant.
- **Far systems are inert.** `galaxy_map_update_orbits` advances only materialised systems, so
  orbital motion literally does not run outside the camera's neighbourhood.
- **Station layout must be reproducible two ways.** `galaxy_node_station_layout` regenerates a
  system's stations from the *lightweight node summary alone* — explicitly so the macro mission
  layer can locate stations without materialising the system — and must produce identical
  positions to `generate_system_stations`, which materialisation uses. Two code paths, one
  required result, no assertion. `galaxy_map.h` names both.
- **Station spawn policy reads a *frozen* ownership snapshot.** `station_near_habited_gen` uses
  `node_owner_gen` rather than live borders, explicitly so an uninhabited system's station set
  stays stable for the session regardless of how the history simulation shifts territory. Two
  ownership arrays with different purposes.
- **`station_mix` is an independent PRNG stream** so station-layout rolls never perturb
  star-system generation determinism — stated in `sim/galaxy_map.cpp`.
- `galaxy_route_find` relies on the MST spanning tree for its connectivity guarantee, a property
  established in `sim/galaxy_gen.h`.
- **Two functions declared in `state/game_state.h` are defined here** —
  `sensor_visibility_from_dist` and `get_sensor_visibility` — which is why three render
  subsystems call them without including this header.

**Extension points:** **A new per-system entity** follows the station pattern: a fixed array on
`StarSystem`, a deterministic generator called during materialisation, and — if the macro tier
needs to locate it without materialising — a summary-only reproduction function alongside
`galaxy_node_station_layout`. **A new galaxy-scale query** belongs here as a pure read over
nodes and the lane graph, following `galaxy_nearest_node` / `galaxy_lane_length`. **A new
generation stage** is a case in `run_generation_stage` (`game.cpp`) plus a function here; the
`worldgen` / `finalize` split exists specifically so the New Game progress bar can advance
between heavy steps, with GalaxyHistory's stages running in between.

**Known limitations / tech debt:**
- **The hot cache is a fixed 64 slots with no documented behaviour when full.**
  `galaxy_ensure_materialized` returns -1 for an invalid node "or when the cache is full", and
  callers must handle that; nothing indicates how often it happens in practice.
- **Cache slots are frame-scoped indices handed out widely.** Eight subsystems consume this
  header, and the slot-vs-node distinction is enforced only by comments.
- Two ownership arrays (`node_owner` live, `node_owner_gen` frozen) with subtly different
  meanings and no naming that makes the distinction obvious at a call site.
- `galaxy_route_find` allocates scratch through `bs_memory_allocator` **per call** rather than
  keeping a persistent buffer.
- A `BS_LOG_INFO` marked "temp Phase 0 verification" is still in the file.
- The station spawn rule (full set when habited now; otherwise a 10% deterministic roll, +10%
  within 1–2 lane hops of a system habited at generation, and half the count when it does spawn)
  is a documented policy implemented as inline conditionals with magic percentages.
- `using GalaxyState = game_state::GalaxyState` — the galaxy state is a *nested* struct of the
  god struct, so this module must alias it to name it at all.
- The two-hop "near habited" search walks the lane graph's CSR adjacency inline rather than
  through a helper, so the traversal is duplicated wherever else it might be needed.
- **In a 1/1 cycle with GalaxyHistory**: this module seeds and reads history ownership while
  `galaxy_history.cpp` calls `galaxy_nearest_node`. Mild and expected for a hot cache over a
  simulated galaxy, but real.

**Source paths:** `sandbox/source/sim/galaxy_map.{cpp,h}`

**Last verified:** 2026-08-07, commit `812680c`
