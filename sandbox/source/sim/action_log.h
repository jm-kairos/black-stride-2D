#pragma once
#include <defines.h>

struct game_state;

// =====================================================================================
// Action log: a rolling 30-entry HUD message buffer with an inactivity fade.
//
// action_log_push appends a printf-formatted message (oldest evicted at capacity) and resets
// the fade timer. It operates on game_state's embedded action_log buffer. The buffer is rendered
// by the RmlUi HUD (game_push_hud), which replaced the old bs_ui action-log panel.
// =====================================================================================
void action_log_push(game_state* s, const char* fmt, ...);
