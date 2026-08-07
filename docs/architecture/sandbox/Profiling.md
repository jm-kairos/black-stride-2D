# Profiling

**Responsibility:** Owns per-frame CPU timing for the game's major subsystems — a fixed table of
named zones, accumulation and rolling averages, ingestion of engine-supplied frame timings, and
the panel body that displays them. It explicitly does not own the panel *window* (DevPanels
opens it and delegates the body), does not own the engine-side timings it displays (it receives
them through setters), and does not measure GPU work — everything here is CPU wall time from
`std::chrono::steady_clock`.

**Public interface:** `sandbox/source/core/profiler.h` — `enum ProfileZoneId` (15 zones plus
`PROF_ZONE_COUNT`); `struct ProfileZone`; `struct Profiler` with `init`, `begin_frame`,
`set_wall_dt`, `set_present_ms`, `set_present_breakdown`, `begin`, `end`, `build_ui`;
`struct ScopedProfile` and the `BS_PROFILE(prof, id)` RAII macro.
The struct is a `game_state` member, so it is reached through the hub rather than by include —
no other subsystem includes this header directly.

**Depends on:** engine `renderer/bs_ui.h` (panel body), `renderer/renderer.h`
(`renderer_set_present_mode`), `defines.h`.
**Depended on by:** nothing by include. Instrumentation call sites exist in FrameOrchestrator,
InWorldOverlays, ShipRendering, SceneOrchestration and GalaxyMapRendering, all reaching it via
`s->profiler`.

**Key invariants:**
- **`begin_frame` finalises the *previous* frame** rather than requiring an `end_frame`. The
  header states this is deliberate, so there is no ordering dependency between the update and
  render passes. It must still be called exactly once per frame, at the top of `game_update`.
- **The three group labels are compared by pointer identity.** `G_FRAME`, `G_UPDATE` and
  `G_RENDER` are file-static `const char*` in `core/profiler.cpp`, and `build_ui` detects
  group changes with `z.group != cur_group`. The comment explains why: string-literal pooling is
  not guaranteed across separate occurrences. Grouping therefore breaks if a zone is assigned a
  freshly-written literal instead of one of those three pointers.
- **`PROF_ZONE_COUNT` sizes the table and `init` populates names positionally**, so inserting a
  zone mid-enum silently relabels every later one. Unenforced.
- Every ingested value is filtered through a plausibility window (discarded above 1000 ms) so
  first-frame spikes and debugger breakpoints cannot poison the rolling averages. The same
  0.9/0.1 EMA is applied to five separate quantities.
- `begin`/`end` do not bounds-check `id`, and a mismatched pair silently corrupts the
  accumulator rather than failing.

**Extension points:** Adding a zone is three coordinated edits: a value in `ProfileZoneId`
(before `PROF_ZONE_COUNT`), a name and group assignment in `Profiler::init`, and instrumentation
at the call site — either `BS_PROFILE(&s->profiler, PROF_X)` for a whole scope or an explicit
`begin`/`end` pair around a single call. Both styles are in use. The group must be one of the
three existing static label pointers or the UI grouping breaks.

**Known limitations / tech debt:**
- **`build_ui` mutates engine renderer state.** Its "GPU mode" button calls
  `renderer_set_present_mode` and only mirrors `present_immediate` when the call succeeds
  (because a driver may refuse IMMEDIATE) — so a profiler panel is a control surface for the
  swapchain, not just a readout.
- **It uses `std::chrono::steady_clock`, not the engine's clock**, and the header explains why:
  `platform_get_absolute_time` is engine-internal and not exported. So the game and the engine
  measure time from two different sources, and the numbers displayed side by side in the panel
  come from different clocks.
- `ScopedProfile` does not null-check `p`, so `BS_PROFILE(nullptr, …)` crashes in the
  constructor — the macro's convenience hides the dereference.
- The struct mixes three concerns: timing data, UI state (`expanded`), and a mirror of engine
  state (`present_immediate`, documented as a "UI mirror").
- Zones with no recorded time are skipped in the UI, so the panel's row set changes frame to
  frame depending on what ran.
- **The zone list is a map of the game's intended decomposition** — fleet autopilot, fleet sim,
  ship collision, projectiles, travel, RTS controls, out-sensor FX, stars, background, ships,
  heat map, UI — recorded here before any of those subsystems is named as such anywhere else.
  Several no longer match the current module boundaries (there is no `PROF_` zone for galaxy
  history, missions, or the economy, which are among the most expensive systems).
- Nothing profiles the generation pipeline, despite `system_evolution.h` documenting a ~30–50 µs
  per-system budget across ~10,000 nodes.

**Source paths:** `sandbox/source/core/profiler.{cpp,h}`

**Last verified:** 2026-08-07, commit `812680c`
