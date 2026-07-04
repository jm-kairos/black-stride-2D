#pragma once
#include <defines.h>

struct game_state;

// =====================================================================================
// Action log: a rolling 30-entry HUD message buffer with an inactivity fade.
//
// action_log_push appends a printf-formatted message (oldest evicted at capacity) and resets
// the fade timer. It operates on game_state's embedded action_log buffer. The bottom-right HUD
// panel that renders this buffer (build_action_log_panel) lives in ui/action_log_panel.h.
// =====================================================================================
void action_log_push(game_state* s, const char* fmt, ...);
