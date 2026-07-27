# Epoch-Based Planetary Evolution Model

Authoritative architecture notes for the star-system evolution pipeline. Consult this before
touching anything under `sandbox/source/sim/system_evolution.*`, the evolved-body data model
in `state/game_state.h`, or any consumer of `StarSystem.evo`.

## Purpose & design constraints

Star systems are not rolled in a single pass; each is the output of a **simulated formation
history**. Instead of real-time N-body physics, the model advances a system through
**macro-time epochs** across four chronological phases, transforming initial stellar
conditions into a fully realized system with deep internal consistency (composition follows
from formation location, atmospheres from volcanism and stellar radiation, habitability from
everything upstream) and emergent gameplay data (resource richness, hazard ratings, moons,
belts, a per-body chronicle).

Hard constraints the implementation honors:

- **Deterministic pure function** of `(seed, StarProperties)`. Same inputs → bit-identical
  `EvolvedSystem` (verified by the selftest).
- **Independent RNG streams** — every draw is keyed `(seed, body slot, epoch)` via
  `evo_rng()`, so a conditional draw for one body never perturbs another body's outcomes.
  Fate decisions use a `+100` epoch salt, moon rolls `+200`, geophysics `+300`, synthesis `500`.
- **Fixed-size stack data only** — no allocation. `EvolvedSystem` is a POD embedded in
  `StarSystem`.
- **Budget ~30–50 µs/system** — `fill_node_summary` runs the full pipeline for every galaxy
  node during map generation, and `generate_star_system` re-runs it on materialisation.
- Epoch numbering is continuous across phases for the chronicle:
  `0` = disk, `1..8` = accretion, `9..14` = geophysics, `15` = today.

## Files

| File | Role |
|---|---|
| `sandbox/source/sim/system_evolution.h/.cpp` | The four-phase pipeline + selftest |
| `sandbox/source/state/game_state.h` | Data model: `EvolvedSystem`, `EvolvedBody`, `BodyComposition`, `EvolutionEvent` |
| `sandbox/source/sim/ss_generation.cpp` (`generate_star_system`) | Drives evolution, derives render views + visual genome |
| `sandbox/source/ui/system_inspector.cpp` | ImGui debug inspector over the evolved state |
| `sandbox/source/game.cpp` (planet-inspector fill block) | RML in-game window presenting evolved data to the player |

## Data model (`game_state.h`)

```
BodyComposition { metal, silicate, ice, gas }      // mass fractions, always sum to ~1
EvolvedBody {
    kind (STAR/PLANET/MOON/BELT), parent (bodies[] index), type (PlanetType),
    orbit_au, width_au (belts), eccentricity, mass_earth, radius_earth, temperature_k,
    comp, water_frac, atmo_pressure (atm), magnetic_field, tectonics, volcanism,
    life, habitability, env_hazard, res_metal, res_volatiles                 // all 0..1 except noted
}
EvolutionEvent { epoch, kind, body, other }        // body/other: bodies[] index, -1 = n/a,
                                                   // -2 = a protoplanet that did not survive
EvolvedSystem { bodies[], body_count, planet_count, moon_count, belt_count, events[], event_count }
```

**Body array layout is a contract**: `[0]` = star, `[1..planet_count]` = planets sorted by
semi-major axis, then moons (with `parent` pointing at their planet's index), then belts.
All consumers (render views, tooltips, inspectors, chronicle) index on this order.

## The four phases

### Phase 1 — Disk condensation (`phase1_disk`)

Builds a 1-D protoplanetary disk profile from the star and seeds 5–10 protoplanet cores
(`ProtoCore` working records, lunar-to-Mars scale embryos at 0.02–0.12 Me):

- Cores sit at **log-spaced, jittered orbits** across `worldgen_orbit_range_au` (tight inner
  packing, sparse outer).
- Each core owns its **annulus feedstock budget**: surface density ~ a^-1.5 × metallicity ×
  disk mass scale (3.5–8.0 × star mass × metallicity); with log spacing the annulus budget
  falls as ~a^-0.5.
- The **frost line** (`star.frost_line_au`) splits compositions: inner cores are
  metal/silicate (metal fraction scales with stellar metallicity), outer cores are ice-rich
  (`{0.10, 0.32, 0.58, 0}`) and their budget is multiplied ×4 (ice condenses).

The core's **slot index is its RNG identity for the whole run** — merged/ejected cores keep
their slot so surviving bodies' streams never re-key.

### Phase 2 — Accretion & migration (`phase2_accretion`, 8 epochs)

A fixed epoch loop; the gas disk disperses at a rolled epoch 4–6 (−2 for stars >1.5 M☉,
which photo-evaporate their disks fast). Per epoch:

1. **Growth** — each core eats 35–55% of its remaining feedstock.
2. **Runaway gas accretion** — a core reaching ≥6 Me *while gas remains* becomes a giant,
   gaining 1.5–6× its mass in gas *per epoch remaining before dispersal* (earlier runaway →
   fatter giant). Logs `GIANT_FORMED`.
3. **Migration** — giants surf the gas inward (type II, ×0.80–0.90/epoch, floored at
   0.55×au_min; logs `MIGRATED` once); solid cores >1.5 Me drift slowly (type I,
   ×0.88–0.98/epoch). Both stop at disk dispersal.
4. **Giant stirring** — a giant's gravitational stirring starves non-giant cores within
   12 Hill radii (feedstock ×0.25). This is what keeps giant neighbourhoods barren.
5. **Hill-stability pass** — adjacent alive pairs (sorted by a) closer than 6 mutual Hill
   radii are unstable: the lighter body is **ejected** (45% chance if the heavier is a giant
   >3× its mass and the system can spare it) or **merged** (mass-weighted composition and
   orbit; logs `MERGER`/`EJECTED`). A post-dispersal merger onto a 0.3–8 Me rocky survivor
   has a 65% chance to spin off an **impact moon** (`MOON_IMPACT`). One resolution per epoch.
6. **Belt formation** — after dispersal, a runt (<3 Me, <5% of the giant's mass) still being
   stirred within 14 Hill radii of a giant grinds down into an **asteroid belt** instead of
   growing (`BELT_FORMED`; width clamped to 4–22% of its orbit radius).

### Phase 2.5 — Body array assembly (`build_body_array`)

- Star into `[0]`; surviving cores sorted by a into `[1..pc]`.
- **Count clamps**: >`MAX_SYSTEM_PLANETS` survivors → lightest merged into its neighbour;
  <2 survivors (heavy ejection runs) → deterministic rocky backfill near the habitable zone.
- **Moons**: giants roll 1–2 *captured* icy moons (0.002–0.03% of parent mass); flagged
  rockies get 1 *impact* moon (0.8–2% of parent mass, mantle-like composition). Moon orbits
  sit at 8–35% of the parent's Hill radius.
- **Belts** appended last.
- **Chronicle remap**: accretion events logged with ProtoCore slot indices are remapped to
  final `bodies[]` indices (`-2` = a protoplanet that did not survive) so the per-planet
  chronicle in the UI can attribute them.

### Phase 3 — Geophysics & atmosphere (`phase3_geophysics`, 6 epochs)

Epochs march through the **star's actual age** (`age_step = age_gyr / 6`). Giants short-
circuit (bottomless atmosphere, strong dynamo). For each solid body per epoch:

- **Differentiation (epoch 0)**: metals sink → core dynamo; `magnetic_field ∝ comp.metal ×
  √mass`.
- **Interior heat** decays as `exp(-age / tau)` with a **mass-dependent cooling timescale**
  `tau = 2 + 2.5·√mass` — small worlds (Mars) freeze out in a couple of Gyr, super-Earths
  stay geologically active for the age of the galaxy. `tectonics`/`volcanism` track heat
  (+ **tidal heating** for close-in moons of massive parents).
- **Atmosphere**: volcanic outgassing (scaled by silicate/ice content and mass) races
  thermal Jeans escape + stellar **XUV stripping** (cool M/K dwarfs are far harsher, 0.55 vs
  0.10 for G-types; magnetic field shields ×(1−0.7·B); young stars flare hardest — XUV drops
  ×0.35 from the third epoch). A once-thick atmosphere dropping below 0.05 atm logs
  `ATMO_STRIPPED`.
- **Water (epoch 1)**: late heavy bombardment delivers outer-system ice inward — the
  delivered fraction scales with `√(ice reservoir beyond the frost line) / a`. Native
  `comp.ice` also contributes. Hot worlds (>380 K) boil it off unless a thick atmosphere
  holds steam. Logs `WATER_DELIVERED`.
- **Greenhouse**: `T = T_eq · (1 + 0.14·ln(1 + P_atm))`.
- **Life**: worlds with liquid water (>0.15), clement temperature (240–330 K) and a
  moderate atmosphere (0.2–8 atm) accumulate biosphere over the ages, sheltered by the
  magnetic field. Logs `LIFE_EMERGED` past 0.3.
- **Dynamo decay (end of phase)**: the magnetic field is scaled by residual core heat
  (`× clamp01(0.25 + 1.4·heat_final)`) — a cooled-out world cannot keep a strong field.

### Phase 4 — Present-day synthesis (`phase4_synthesis`)

- **Classification** (`classify_body`) from the evolved state, in priority order: gas >0.35 →
  GAS_GIANT/ICE_GIANT (mass/ice split); T>700 K or violent volcanism → LAVA; T<200 K →
  FROZEN; water>0.55 + atmosphere → OCEAN; water>0.15 + atmosphere + 240–330 K → TERRAN;
  warm + air + dry → DESERT; else ROCKY.
- **Radius** from mass + composition (rocky `m^0.27` with ice puff; giants clamped 3–13 Re).
- **Habitability**: multiplicative gates (TERRAN/OCEAN only): temperature fit × atmosphere
  fit × water fit × magnetic shelter × life bonus.
- **env_hazard**: max-blend of radiation (XUV / distance², shielded by the dynamo), impact
  flux near belts, extreme volcanism, and crushing atmospheres.
- **Resources** (data only; markets read them downstream): `res_metal ∝ comp.metal ×
  (1.2 + volcanism)` (volcanism concentrates ores), `res_volatiles ∝ comp.ice + water/2`.
  Belts get boosted metal/ice richness and a collision-flux hazard.

## Downstream consumption (`generate_star_system`, ss_generation.cpp)

- `evolve_star_system()` is the **authoritative model**; everything else is a *view*:
  - **Render orbits** are a log-map of the evolved AU orbits into bounded world units (real
    AU spacing would not fit galaxy spacing); non-intersection is enforced on top.
  - **Moon render orbits** are presentational (parent render-radius multiples) — real Hill
    distances would be sub-pixel.
- `PlanetProperties` carries the player-facing view per planet: type, orbit/mass/radius/
  temperature, habitability, **water_frac**, **life**, atmosphere/ring flags, and the visual
  **genome**.
- **Visual genome & trait reconciliation** (important invariant): the subtype archetype roll
  (`worldgen_planet_genome`) is weighted by evolved reality — Oceanic-tagged subtypes by
  `water_frac`, Verdant-tagged by `life`, Arid-tagged by dryness. After the roll, all
  **physical-claim trait bits** (`OCEANIC | VERDANT | ARID | ICY_CAPS | VOLCANIC | METALLIC |
  CRATERED`) are cleared and **re-derived from the evolved body** for planets *and* moons;
  only purely visual bits (`CLOUDY/STORMY/BANDED/EXOTIC`) survive from the archetype/anomaly.
  Never let an archetype table re-introduce a physical claim the evolution contradicts.

## Chronicle (event log)

`EvolutionEvent` kinds: `DISK_DISPERSED, GIANT_FORMED, MIGRATED, MERGER, EJECTED,
BELT_FORMED, MOON_IMPACT, MOON_CAPTURED, ATMO_STRIPPED, WATER_DELIVERED, LIFE_EMERGED`
(labels via `evo_event_name`). The planet inspector shows each body's events plus
system-wide ones (`body == -1`); events referencing `-2` concern lost protoplanets.

## Validation — selftest (`system_evolution_selftest`)

Runs at `game_init` in debug builds (always on for the sandbox). Evolves 64 seeds and logs
`[evo selftest]` statistics plus invariants — **all failure counters must stay 0**:

- `determinism_fail` — two runs of the same seed must be bit-identical (`memcmp`).
- `count_fail` — body-count bookkeeping (`body_count == 1 + planets + moons + belts`, 2..MAX planets).
- `comp_fail` — every composition sums to 1 ± 0.02.
- `hill_fail` — adjacent planets ≥3 mutual Hill radii apart, orbits strictly ordered.
- `moon_fail` — moons inside their parent's Hill sphere.
- `finite_fail` — every float field finite.
- `massive_dead` / `dead_strong_dynamo` — geophysics consistency: no solid world >2 Me with
  dead geology; no dead world with a strong dynamo.

Distribution sanity (retune constants if these drift badly): habitable ~1–5%, giants
present, oceans/terrans nonzero, no planet type dominating the histogram. A full epoch
trace for one seed follows the stats.

**How to capture the log on Windows**: the game logs to console + `OutputDebugStringA`
only (no file). Run `bin\sandbox.exe` from `bin\` and watch the console, or attach a
DBWIN_BUFFER listener / DebugView.

## Tuning guardrails (learned the hard way)

- Growth rate, disk mass scale, runaway threshold, and water delivery were tuned together —
  they interact. E.g. lowering the runaway threshold (6 Me) creates more giants → more
  stirring → fewer terrans and more belts.
- The greenhouse term feeds back into water retention and life gates; atmosphere constants
  shift the OCEAN/TERRAN/DESERT balance strongly.
- Anything touching per-body draws must preserve the **stream keying** discipline: never
  make one body's number of RNG draws depend on another body's outcome within the same
  stream. Add a new salted stream instead.
- After any tuning, rebuild and check the selftest block: invariants zero, distributions
  within the sanity envelope above.
