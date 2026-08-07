# DeterministicRng

**Responsibility:** Owns the seed hierarchy that makes worldgen reproducible — a stateless
splitmix64 bit-mixer, an index-invariant per-index seed derivation, and a small stateful stream
for a single generation pass. It explicitly does not own any generated content, and it does not
own *all* the randomness in the game: three other independent generators exist (see tech debt),
and this header is deliberately only the galaxy-scale one.

**Public interface:** `sandbox/source/sim/galaxy_rng.h` — `galaxy_splitmix64(u64)`,
`galaxy_seed_for(u64 master, i32 index)`, `struct GalaxyRng`, `galaxy_rng_seed`,
`galaxy_rng_next`, `galaxy_rng_f32`, `galaxy_rng_range`, `galaxy_rng_int`. Header-only; every
function is `static inline`.

**Depends on:** engine `defines.h` only. It is the sandbox subsystem with the fewest
dependencies.
**Depended on by:** GalaxyGeneration, GalaxyHistory, DevPanels.

**Key invariants:**
- **`galaxy_seed_for` must stay index-invariant.** It hashes
  `master ^ (0x9e3779b97f4a7c15 * (index + 1))` specifically so that adding or removing systems
  never disturbs an unrelated index's seed — stated in the header comment. That property is what
  lets GalaxyRuntime stream systems in and out of its hot cache and get byte-identical content
  each time. Enforced by the formula alone; nothing tests it.
- **The whole galaxy derives from one master seed** via master → per-node → per-body. Documented
  in the header; the per-node level is `galaxy_seed_for`, and the per-body level is delegated
  (see below).
- Determinism depends on these functions never gaining state beyond the explicit `GalaxyRng`
  argument. All are pure by construction.

**Extension points:** A new generator seeds a `GalaxyRng` from a node seed via
`galaxy_rng_seed(galaxy_seed_for(master, index))` and draws with `galaxy_rng_f32` /
`_range` / `_int` — the pattern `sim/galaxy_gen.cpp` and `sim/galaxy_history.cpp` both follow.
If a new subsystem needs rolls that must **not** perturb an existing stream, the established
convention is to keep a private splitmix64 seeded from the shared seed rather than sharing a
`GalaxyRng` — `station_mix` in `sim/galaxy_map.cpp` does exactly this, with a comment explaining
that it stays independent of the star-system RNG so it never perturbs planet generation.

**Known limitations / tech debt:**
- **There are four independent splitmix64 implementations in the sandbox**, only one of which is
  this header: `station_mix` in `sim/galaxy_map.cpp` (deliberate, documented), `sm64` in
  `sim/ai_ship.cpp` (described in-file as "deterministic-ish"), and `sm64` in
  `sim/ship_mission.cpp`. `sim/ss_generation.cpp` keeps a fifth equivalent copy, which
  `galaxy_rng.h` explicitly sanctions so a node's contents are fully determined by its seed.
  Some of that duplication is principled; none of it is shared.
- A sixth, unrelated generator exists in the dead `render/starfield_generator.cpp` — a plain LCG.
- `galaxy_rng_f32` uses only the low 24 bits of the draw, and `galaxy_rng_int` uses modulo, so
  both carry mild distribution bias. Acceptable for content generation, but worth knowing before
  using them for anything statistical.
- All functions are `static inline` in a header, so every including translation unit gets
  private copies — harmless, but it means the generator cannot be swapped at link time.
- **The master seed is not actually player-controlled today.** `sim/galaxy_map.cpp` holds a
  file-static `GALAXY_MASTER_SEED` constant that duplicates the value `game_init` writes into
  `s->setup.seed`, so the two coincide; the New Game screen's "Randomize seed" button
  (`ui/new_game_setup.cpp`) rerolls `s->setup.seed` through `galaxy_splitmix64`. *Inferred:*
  that the file-static is a leftover from before the setup screen existed — the code does not
  say.
- Nothing here is used by the engine; this is a game-side primitive despite sitting alongside
  engine-facing headers.

**Source paths:** `sandbox/source/sim/galaxy_rng.h`

**Last verified:** 2026-08-07, commit `812680c`
