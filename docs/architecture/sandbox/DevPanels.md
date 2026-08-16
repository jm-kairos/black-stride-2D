# DevPanels

**Responsibility:** Owns the game's ImGui panels — the large multi-section editor panel, the
transform window for the selected entity, the profiler readout, the New Game setup screen with
its generation progress bar, and the System Inspector showing a star system's evolved-body model
and formation chronicle. It explicitly does not own the shipping HUD (that is RmlUi, driven from
FrameOrchestrator's `game_push_hud`), does not own the widget vocabulary (the engine's `bs_ui`),
and does not own the state it edits — it writes into `game_state` and into three other
subsystems' globals.

> **Grouped by decision.** The three files share **zero include edges** with each other; they
> cohere only by all building `bs_ui` panels called from `game_render`. See
> `sandbox-subsystems.md` §Settled calls.

**Public interface:** `sandbox/source/ui/editor_ui.h` — `build_editor_panel`,
`build_transform_panel`, `build_profiler_panel`.
`sandbox/source/ui/new_game_setup.h` — `build_new_game_setup`, `build_generation_progress`.
`sandbox/source/ui/system_inspector.h` — `build_system_inspector`.
All six are called **only** by FrameOrchestrator.

**Depends on:** GalaxyGeneration, DeterministicRng, CoordinateDiagnostics, CoordinateFrames,
ActionLog, CombatArena, ShipRendering, InWorldOverlays, Profiling, GameStateModel; engine
`renderer/bs_ui.h`, `renderer/bs_rml.h`, `core/logger.h`.
**Depended on by:** FrameOrchestrator.

**Key invariants:**
- **Panels are built during `game_render` but edit state consumed by the next `game_update`** —
  a one-frame lag inherent to editing during render. Nothing surfaces it.
- **Begin/end must be paired even when begin returns FALSE** (the engine's `bs_ui` rule);
  violating it corrupts ImGui's window stack at runtime with no compile-time signal.
- **The "Multiple ship command" checkbox must maintain the combat-entity window.** Toggling it
  calls `Fleet::set_count` *and* `combat_arena_rebuild_player_entities`, because the NPC entity
  window is packed after the player window and `npc_combat_base` must be recomputed. A checkbox
  is responsible for a cross-subsystem invariant.
- **The "Generate Galaxy" button is the application phase transition.** It flips
  `s->app_phase` to `APP_GENERATING` and resets the stage counter, handing control to
  `run_generation_stage` in `game.cpp`.
- **`PLANET_EDITOR_TYPE_COUNT == PLANET_TYPE_COUNT`** is enforced by a `static_assert` in
  `render/star_fx.cpp`, which is what keeps the Planet Editor's rows aligned with the type enum
  (that window is owned by CelestialFx, launched from a button here).
- The System Inspector suppresses water and atmosphere gauges for gas-dominated bodies
  (`comp.gas > 0.35`) — a presentation rule derived from the physical model.

**Extension points:** A new editor control is a widget call inside the relevant section of
`build_editor_panel`, writing straight into `game_state` or an `extern` global. A new panel is a
`build_*` function in `ui/`, a `#include` in `game_modules.h`, and a call in `game_render`
(usually behind a `s->…show_*` flag). A new setup option is an entry in the `*_ITEMS`
`\0`-separated combo table, a matching `*_VALUES` array if the stored value is not the index,
and a field on `GalaxySetupParams`; `value_to_index` handles restoring a selection from a raw
number. A new inspector section reads `EvolvedSystem` and uses the local `gauge` / `comp_line` /
`hazard_badge` helpers.

**Known limitations / tech debt:**
- **`build_editor_panel` is a single ~640-line function** covering edit mode, lights, glow,
  bloom, background layers, coordinate diagnostics, nebula, camera, starfield, travel and system
  view — the widest single reach into `game_state` anywhere in the codebase.
- **It is the writer for three globals other subsystems own**: `g_debug_cell_grid`
  (CoordinateDiagnostics), `g_zoom_out_speed_gain` (CoordinateFrames) and
  `g_sensor_fade_distance` (InWorldOverlays). Those `extern` declarations exist for this file.
- **Two of the three panels request anchoring that is silently ignored.**
  `build_editor_panel` and `build_transform_panel` pass `BS_UI_TYPE_EDITOR`, and the engine's
  `bs_ui_begin_panel` applies anchoring and window flags only for `BS_UI_TYPE_GAME` — so their
  anchor and margin arguments have no effect.
- **Zero cohesion between the three files.** No include edges, no shared helpers, three
  unrelated purposes. The grouping is by call site and engine dependency only.
- The combo tables use ImGui's `"A\0B\0C\0"` convention passed through `bs_ui_combo`, despite
  `bs_ui.h` claiming to hide ImGui.
- `comp_line` in `system_inspector.cpp` accumulates into a fixed 128-byte buffer with running
  `snprintf` offsets and no re-check against remaining space — the same pattern as the engine's
  memory-usage string.
- Hazard severity thresholds (0.25, 0.55) are duplicated here as display bands with no shared
  constant.
- The `civ_density` combo is labelled "Dynastic Houses" while the field is named for density,
  reflecting a renamed concept.
- The generation cost estimate is a heuristic (`size × depth × civ_density × detail`) mapping to
  Fast/Moderate/Slow/Very slow; it encodes which parameters drive generation time but is not
  derived from any measurement.
- Several panels here are the *only* consumer of an entire subsystem's output — the System
  Inspector is the sole reader of the evolution chronicle, so 775 lines of
  `system_evolution.cpp` are player-visible in exactly one window.

**Source paths:** `sandbox/source/ui/**`

**Last verified:** 2026-08-07, commit `812680c`
