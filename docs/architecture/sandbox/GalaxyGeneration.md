# GalaxyGeneration

**Responsibility:** Owns one-shot worldgen — placing ~10,000 star-system nodes on a structured
disc, deriving each node's deterministic summary, building the spatial index and the travel-lane
graph, and generating the contents of an individual star system by simulating its four-phase
formation history. It explicitly does not own the *runtime* galaxy (GalaxyRuntime owns the hot
cache, orbital updates and routing), does not own civilizations (GalaxyHistory consumes the
habitability substrate this produces), and does not own the seed hierarchy (DeterministicRng).

*(Node placement and per-system generation were briefly separate subsystems; the split produced
a cycle because `SSGenEnv` is declared in `galaxy_gen.h` and consumed by `ss_generation.h`. See
`sandbox-subsystems.md` §Settled calls.)*

**Public interface:** `sandbox/source/sim/galaxy_gen.h` — the placement tunables
(`GALAXY_TARGET_SYSTEMS`, `GALAXY_DISC_RMAX`, `GALAXY_MIN_SEPARATION`, `GALAXY_GRID_CELL`,
`GALAXY_KNN_K`, plus nine spiral-structure constants); `struct SSGenEnv`;
`galaxy_params_for_shape`, `galaxy_arm_angle`, `galaxy_env_at`, `galaxy_generate`, `galaxy_free`.
`sandbox/source/sim/galaxy_params.h` — `enum GalaxyShape`, `struct GalaxyGenParams`.
`sandbox/source/sim/galaxy_spatial.h` — `struct GalaxySpatialGrid`, `galaxy_grid_build`,
`_free`, `_nearest`, `_query_radius`.
`sandbox/source/sim/ss_generation.h` — `SSGenConfig`, `generate_star_system`,
`update_planet_positions`, `solve_eccentric_anomaly`, `worldgen_star`, `worldgen_planet`,
`worldgen_planet_genome`, `worldgen_orbit_range_au`, the label helpers, `blackbody_color`.
`sandbox/source/sim/system_evolution.h` — `evolve_star_system`, `evo_event_name`,
`system_evolution_selftest`.
Used from outside: `ss_generation.h` by 4 subsystems, `system_evolution.h` and `galaxy_gen.h`
and `galaxy_spatial.h` by 2 each, `galaxy_params.h` by 1.

**Depends on:** DeterministicRng, GameStateModel; engine `core/memory/bs_memory.h`,
`math/bs_hierpos.h`, `core/logger.h`, `defines.h`.
**Depended on by:** GalaxyRuntime, GalaxyMapRendering, DevPanels, GameStateModel,
FrameOrchestrator.

**Key invariants:**
- **Systems must not physically overlap.** `galaxy_gen.h` shows the arithmetic: typical
  neighbour spacing must exceed a star system's outer-orbit *diameter* (~4.0e8), so
  `GALAXY_MIN_SEPARATION` is set to ~4× that "so interstellar space reads as EMPTY relative to
  system size", enforced by blue-noise rejection during placement. Orbits themselves are
  non-intersecting by construction: `SSGenConfig`'s spacing factor (1.40–2.00) plus a 5% safety
  margin between inner apoapsis and outer periapsis.
- **`SSGenEnv` must be a pure function of position.** This exists to guarantee one specific
  property, spelled out in `galaxy_gen.h`: a system's summarised map dot (generated from its
  true world position) and the system the player later flies into (materialised from `{0,0}`
  then re-anchored for precision) must derive the *same* star population and colour.
- **A node's summary must match what materialisation later produces.** `fill_node_summary`
  generates a full `StarSystem` and discards it, keeping only colour, radius, sorted orbit radii
  and habitability — safe because the same seed plus env re-derives the identical system. The
  comment states this explicitly.
- **Node 0 is forced to the origin ("Sol")** so the player start stays valid, which is why
  `game_init` sets `camera_hierpos` to `{0,0}` before generation.
- **System names cannot collide.** `galaxy_system_name` uses an LCG permutation
  `(i*48271 + 12345) mod 676000`, a bijection because 48271 is coprime to the modulus — the
  comment notes a plain hash would collide birthday-paradox often. Correctness rests on that
  number-theory property.
- **The spatial grid is immutable after build**, which `galaxy_spatial.h` says makes all queries
  "safe to call lock-free from any thread". Built once in `galaxy_generate`, never mutated.
- **Per-body/per-epoch RNG salts keep body streams independent** so one body's outcome never
  reorders another's (`system_evolution.h`) — the same index-invariance principle
  DeterministicRng applies at galaxy scale, applied within a system.
- `evolve_star_system` requires the star to be rolled already so the caller controls the star
  RNG stream — an ordering contract stated in `system_evolution.h`.

**Extension points:** **A new galaxy morphology** is a value in `GalaxyShape` plus a preset
branch in `galaxy_params_for_shape` and, if it needs new geometry, sampling in `place_nodes`;
`GalaxyGenParams` is deliberately a union of all six generators' inputs with unused fields
ignored. **A new planet type** flows through `worldgen_planet` / `worldgen_planet_genome` and
needs a matching `PlanetTypeParams` row in `CelestialFx` (`PLANET_EDITOR_TYPE_COUNT` must equal
`PLANET_TYPE_COUNT`, enforced by a `static_assert` in `render/star_fx.cpp` — one of the few real
compile-time checks in the sandbox). **A new evolution outcome** is an epoch pass plus an
`EvoEventKind` and a label in `evo_event_name`; the event log is rendered by DevPanels'
System Inspector. New invariants belong in `system_evolution_selftest`, which genuinely runs —
`game.cpp` calls it at init under `BS_DEBUG`.

**Known limitations / tech debt:**
- **Generation cost is dominated by evolution.** `system_evolution.h` documents a ~30–50 µs
  per-system budget *because* `fill_node_summary` runs the full four-phase pipeline for every
  one of ~10,000 nodes — and nothing profiles it (`Profiling` has no zone for generation).
- **`GALAXY_GRID_CELL` (2e9) does triple duty** — spatial-grid bucket size, typical neighbour
  spacing, and the value `sim/fleet.h`'s `JUMP_RADIUS_DEFAULT` is calibrated against. One number
  tying generation, spatial indexing and FTL range across three headers with no shared constant.
- `ss_generation.h` includes `game.h`, pulling the entire 3652-line god struct into a generator
  header — unlike the sibling galaxy headers, which forward-declare.
- The generator works in absolute `f64` world space (the grid converts every node through
  `hierpos_to_f64` on both build passes) rather than the hierarchical frame. Acceptable at
  3.2e11, but it is a second coordinate convention inside the galaxy subsystem.
- `galaxy_grid_query_radius` silently drops candidates past `max` rather than reporting
  truncation.
- The spatial grid assumes a square, power-of-two-friendly field: `tileCols` is reused for both
  axes and grid dimensions derive from the node AABB rather than the configured disc radius.
- `sim/galaxy_map.cpp` holds a file-static `GALAXY_MASTER_SEED` duplicating the value
  `game_init` writes into `s->setup.seed`, so the "player-chosen seed" and a hardcoded one
  currently coincide.
- `SSGenConfig` is a `struct` with default member initialisers and a single `SSGEN_DEFAULT`
  instance; no call site passes a different config, so the parameterisation is unused.
- Habitability is harvested "FREE" from the discarded system and becomes the substrate for
  civilization cradles — a one-byte-per-node coupling between worldgen and deep-time history
  that is easy to miss.

**Source paths:** `sandbox/source/sim/galaxy_gen.{cpp,h}`,
`sandbox/source/sim/galaxy_params.h`, `sandbox/source/sim/galaxy_spatial.{cpp,h}`,
`sandbox/source/sim/ss_generation.{cpp,h}`, `sandbox/source/sim/system_evolution.{cpp,h}`

**Last verified:** 2026-08-07, commit `812680c`
