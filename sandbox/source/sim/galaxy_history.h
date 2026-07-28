#pragma once
#include <defines.h>

struct game_state;

// =====================================================================================
// Galaxy history (Phase 1: civilization origins)
// =====================================================================================
// Macro-scale civilizations, seeded deterministically onto the galaxy's best habitable
// worlds. A civilization is ONE aggregate agent (not a swarm of individuals) so the whole
// history stays tractable + reproducible from the master seed. Phase 1 records only WHERE
// and WHEN each civ arose (its cradle homeworld + founding year); later phases add
// expansion/territory (Phase 2) and a chronicle of events (Phase 3).
//
// Types (Civilization / CivGovernment / CivEthos / GALAXY_CIV_MAX) live in state/game_state.h
// so GalaxyState can embed the civs[] array, mirroring GalaxyNode / GalaxyLaneGraph.

// Seed civilizations into s->galaxy.civs[] (fills civ_count). Deterministic from the galaxy
// master seed. Call once, after galaxy_generate() (needs the per-node habitability substrate).

// Phase B/C deep-time simulation (resumable, chunked so the New Game progress bar can advance by
// simulated year). begin resets + seeds the genesis civs; step advances up to max_steps year-steps
// and returns TRUE once the present is reached. The working-set stays RESIDENT after generation so
// the living present (Phase C) can keep stepping it; finalize_view refreshes the present-day view.
void galaxy_history_sim_begin(game_state* s);
b8   galaxy_history_sim_step(game_state* s, i32 max_steps);
// Refresh the present-day view (territory recount + chronicle sort) from the resident sim state, and
// log a summary. Idempotent/repeatable; the living present (Phase C1) calls this after advancing.
void galaxy_history_finalize_view(game_state* s);
// Release the resident sim working-set (idempotent). Called at (re)generation start.
void galaxy_history_sim_free(game_state* s);

// Phase C1: advance the living present by real time (pace: real-time drift). Steps the resident sim at
// ~1 yr/step, routes new events to the live feed, and refreshes the view. Call each frame in APP_PLAYING.
void galaxy_history_live_tick(game_state* s, f32 dt);
// Emit the one-shot "PhaseB history:" summary (called once at generation end, not per live checkpoint).
void galaxy_history_log_summary(const game_state* s);
// Phase C1: build the live "Galactic News" window (bs_ui). Call each frame while s->galaxy.show_news.
void galaxy_history_build_news(game_state* s);

// Dynastic Houses: build the heredity-tree browser (bs_ui). Call each frame while s->galaxy.show_houses.
void galaxy_history_build_houses(game_state* s);

// Phase C2 (player coupling): the player perturbs the living macro sim. raid weakens a civ + worsens
// your standing; aid improves it. Effects are immediate and surface in the news feed, then propagate.
void galaxy_history_player_raid(game_state* s, i32 civ, f32 strength);
void galaxy_history_player_aid (game_state* s, i32 civ, f32 strength);
i32  galaxy_history_player_rep (const game_state* s, i32 civ);   // this civ's standing toward the player
// Phase C2: build the Live Civ Inspector window (bs_ui). Call each frame while s->galaxy.show_inspector.
void galaxy_history_build_inspector(game_state* s);

// Government interaction windows: themed per-government engagement windows launched from the Live Civ
// Inspector, targeting the civ that owns the player's current system. Call the dispatcher each frame
// while s->galaxy.show_gov_window; it routes to the correct window and auto-closes when the target
// is no longer the current owner (or has fallen). The per-government builders are exposed for testing.
void galaxy_history_build_gov_interaction(game_state* s);
void galaxy_history_build_parliament(game_state* s, i32 civ);
void galaxy_history_build_royal_court(game_state* s, i32 civ);
void galaxy_history_build_synod(game_state* s, i32 civ);
void galaxy_history_build_charter_council(game_state* s, i32 civ);

// Faction Step 1: is `civ` hostile to the player right now? Reputation vs an ethos-adjusted threshold;
// civ<0 (lawless wild space) is hostile. Read each frame to gate faction-patrol aggression.
b8 galaxy_history_is_hostile(const game_state* s, i32 civ);

// Feature B (B-3): public diplomacy queries over the resident relation matrix (pure reads).
// TRUE when civs a and b are currently at war / allied. Out-of-range or a==b -> FALSE.
b8 galaxy_history_civ_at_war(const game_state* s, i32 a, i32 b);
b8 galaxy_history_civ_allied(const game_state* s, i32 a, i32 b);

// Feature B: unified faction stance. faction_id >= 0 is a civ index; negatives are static factions
// (FACTION_PLAYER never hostile; FACTION_PIRATE / wild always hostile). Folds transitive diplomacy:
// an ally's active enemies read hostile to the player.
b8 galaxy_history_faction_is_hostile(const game_state* s, i16 faction_id);

// Phase 1 (autonomous universe): pairwise stance between ANY two unified faction ids. Same faction
// is never hostile to itself; FACTION_PLAYER folds through galaxy_history_faction_is_hostile;
// pirates / wild raiders are hostile to everyone; two civs are hostile only while at war.
// Drives NPC-vs-NPC target acquisition and projectile hit attribution.
b8 galaxy_history_factions_hostile(const game_state* s, i16 a, i16 b);

// Feature B: human-readable label for a faction ("<Civ> Patrol", "Pirate Raider", "Your Fleet").
void galaxy_history_faction_label(const game_state* s, i16 faction_id, char* out, i32 out_size);

// Step B: civilization fleet garrison layer (macro fleet strength per system; player-independent,
// background-simulated). Seeded from ownership at generation, evolved each living tick, read by NPC
// materialisation and written back by player kills.
void galaxy_history_seed_garrison(game_state* s);              // seed from ownership (once, at gen end)
i32  galaxy_history_garrison_at(const game_state* s, i32 node); // rounded fleet strength at a node
void galaxy_history_garrison_add(game_state* s, i32 node, i32 delta); // adjust (player kills, etc.)

// DEBUG: find an inter-civilization border node (an owned node adjacent to a rival's territory),
// scanning after `start_node` (wraps). If the pair is not already at war, FORCE a war between them so
// the frontier grinds (garrison attrition). Returns the border node (or -1), and the two civs via out.
i32  galaxy_history_debug_war_frontier(game_state* s, i32 start_node, i32* out_civ_a, i32* out_civ_b);

f32  galaxy_history_sim_progress(const game_state* s);   // 0..1 across the history window

// Phase B one-shot: run the whole deep-time simulation to completion (begin + step-all + end).
// Deterministic; used by galaxy_map_init. Produces present-day territory + the chronicle.
void galaxy_history_generate(game_state* s);

// Index of the civilization whose homeworld system is galaxy node `node_index`, or -1 if none.
i32 galaxy_history_civ_at_node(const game_state* s, i32 node_index);

// Index of the civilization that CONTROLS galaxy node `node_index` at present, or -1 if unclaimed.
i32 galaxy_history_owner_at_node(const game_state* s, i32 node_index);

// Phase 3 — format one chronicle event into a human-readable sentence (for the Legends browser).
void galaxy_history_event_text(const game_state* s, i32 event_index, char* out, i32 out_size);

// Phase 3 — build the Legends browser window (bs_ui). Call each frame while s->galaxy.show_legends.
void galaxy_history_build_legends(game_state* s);

// Human-readable labels (HUD / tooltip).
const char* civ_government_name(u8 government);
const char* civ_ethos_name(u8 ethos);
// Themed interaction-window title for a government ("Parliament", "Royal Court", ...).
const char* civ_gov_window_name(u8 government);
