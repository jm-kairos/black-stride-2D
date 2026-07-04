#pragma once
#include <defines.h>

struct game_state;

// Bottom-right Action Log HUD panel. Renders the newest 3 messages when idle (fading to 15%
// after a few seconds of inactivity) and expands to the full 30-entry scrollable history on
// hover. Reads game_state's embedded action_log buffer (populated by action_log_push, which
// lives in the sim/ data module). This is the UI/presentation half of the action log.
void build_action_log_panel(game_state* s, f32 dt);
