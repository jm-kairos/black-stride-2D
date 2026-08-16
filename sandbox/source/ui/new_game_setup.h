#pragma once

struct game_state;

// New Game setup screen (Phase A): a centered bs_ui panel that edits s->setup. The "Generate
// Galaxy" button flips s->app_phase to APP_GENERATING to start the staged generation pipeline.
void build_new_game_setup(game_state* s);

// Centered progress panel shown while APP_GENERATING (label + bar from s->gen_label / gen_progress).
void build_generation_progress(game_state* s);
