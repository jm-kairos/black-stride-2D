# GalaxyHistory

**Responsibility:** Owns the civilization layer — seeding civs onto habitable cradles, running a
resumable year-stepped deep-time simulation that produces territory, dynasties, wars and a
chronicle, keeping that simulation living during play, answering every diplomacy and ownership
query the rest of the game asks, maintaining the macro fleet-garrison strength per system, and
building five ImGui browser windows over all of it. It explicitly does not own the galaxy's
physical structure (GalaxyGeneration), does not own the hot cache (GalaxyRuntime), and does not
own its own data types — `Civilization`, `CivGovernment`, `CivEthos` and `GALAXY_CIV_MAX` live
in `state/game_state.h` so `GalaxyState` can embed the arrays.

**Public interface:** `sandbox/source/sim/galaxy_history.h` — ~30 free functions in seven groups:
deep-time staging (`galaxy_history_sim_begin`, `_sim_step`, `_finalize_view`, `_sim_free`,
`_sim_progress`, `_generate`, `_log_summary`); the living present (`_live_tick`);
diplomacy (`_is_hostile`, `_civ_at_war`, `_civ_allied`, `_faction_is_hostile`,
`_factions_hostile`, `_faction_label`); ownership (`_civ_at_node`, `_owner_at_node`);
the garrison layer (`_seed_garrison`, `_garrison_at`, `_garrison_add`);
player coupling (`_player_raid`, `_player_aid`, `_player_rep`);
UI (`_build_legends`, `_build_news`, `_build_houses`, `_build_inspector`,
`_build_gov_interaction` and four per-government builders);
plus `_debug_war_frontier` and the label helpers `civ_government_name`, `civ_ethos_name`,
`civ_gov_window_name`. Used from outside by **7 subsystems**.

**Depends on:** GalaxyRuntime, LocalAgentAi, ActionLog, DeterministicRng, GameStateModel;
engine `renderer/bs_ui.h`, `core/memory/bs_memory.h`, `core/logger.h`, `defines.h`.
**Depended on by:** CombatArena, GalaxyMapRendering, GalaxyRuntime, InWorldOverlays,
LocalAgentAi, MacroMissions, FrameOrchestrator.

**Key invariants:**
- **A civilization is ONE aggregate agent, not a swarm.** `galaxy_history.h` states this as the
  core modelling decision, chosen so the whole history stays "tractable + reproducible from the
  master seed". That single choice is why deep time is simulable at all; abandoning it changes
  the cost model of everything here.
- **The simulation working-set stays resident after generation** so the living present can keep
  stepping the same state. `_sim_free` is idempotent and called at regeneration start. The
  history is a paused simulation, not a precomputed result.
- **Stepping is chunked and resumable purely for the UI.** `_sim_begin` / `_sim_step(max_steps)`
  / `_finalize_view` exist so the New Game progress bar can advance by simulated year — a
  presentation requirement shaping the simulation's control flow.
- **`_finalize_view` is idempotent and repeatable**, which is what lets the living present call
  it after each advance.
- **Two entry points run the same simulation:** `_generate` is a one-shot
  begin/step-all/end wrapper used by `galaxy_map_init`, while the staged path is used by the New
  Game flow. Both must produce the same result from the same seed.
- **`faction_is_hostile` folds transitive diplomacy** — an ally's active enemies read hostile to
  the player — and negative ids encode static factions (`FACTION_PLAYER` never hostile,
  `FACTION_PIRATE`/wild always hostile). This is the game's diplomacy authority, consumed by
  combat hit filtering, NPC target acquisition, patrol labelling and overlay colouring.
- The frozen `node_owner_gen` snapshot must not be mutated after generation — GalaxyRuntime's
  station spawn policy depends on it being stable for the session.

**Extension points:** **A new event type** is a value in `HistoryEventType`
(`state/game_state.h`), an emission via `hist_add` / `live_feed_push`, a case in
`evt_importance`, and a sentence in `galaxy_history_event_text` for the Legends browser. **A new
government** is a `CivGovernment` value, a `civ_aggression` weighting, a themed window builder
following `_build_parliament` / `_build_royal_court` / `_build_synod` /
`_build_charter_council`, and a case in `_build_gov_interaction`'s dispatcher — the four
builders are declared in the header as "exposed for testing", the only place in the sandbox
where visibility is justified that way. **A new player action** follows `_player_raid` /
`_player_aid`: perturb the resident sim, surface it in the news feed, let it propagate.
**A new diplomacy query** should extend the existing three rather than add a fourth fold rule.

**Known limitations / tech debt:**
- **1455 lines, and a large share is presentation.** It builds five ImGui browser windows
  (Legends, News, Houses, Live Civ Inspector, government interaction) directly through
  `bs_ui.h`, so a simulation module owns substantial UI — the same concern-mixing as
  `render/star_fx.cpp` and `core/profiler.cpp`.
- **Three overlapping hostility queries** — `is_hostile(civ)`, `faction_is_hostile(faction_id)`,
  `factions_hostile(a, b)` — each with different fold rules for the player, pirates and wild
  space. Callers must pick the right one; nothing prevents picking wrong.
- **The header doubles as a development log.** Comments carry a phase history (Phase 1 origins →
  Phase 2 territory → Phase 3 chronicle → Phase B deep time → Phase C living present → Phase C2
  player coupling → Feature B diplomacy → Step B garrison), which is useful archaeology but
  means the API is stratified rather than designed as a whole.
- **In a 1/1 cycle with GalaxyRuntime** and a 1/1 cycle with LocalAgentAi.
- Territory and relation arrays are allocated from `MEMORY_TAG_GAME` and the relation matrix is
  mutated through `rel_score_add` with no bounds documentation.
- `_debug_war_frontier` can *force* a war between two civs to make a frontier grind — a testing
  hook exported in the production header and wired to a keybinding in `game.cpp`.
- The garrison layer runs player-independently and is read by NPC materialisation, so agent
  density is an emergent property of deep time — powerful, but it makes "why are there five
  patrols here" hard to answer without instrumenting the history.
- Its types living in `state/game_state.h` means the data and the behaviour are in different
  files, and the god struct is what actually owns the civ arrays.

**Source paths:** `sandbox/source/sim/galaxy_history.{cpp,h}`

**Last verified:** 2026-08-07, commit `812680c`
