#include "sim/action_log.h"
#include "game.h"
#include <stdarg.h> // va_list, va_start, va_end
#include <string.h> // memcpy
#include <stdio.h>  // vsnprintf

#define ACTION_LOG_MAX 30

// Push a formatted message into the rolling 30-entry HUD buffer.
void action_log_push(game_state* s, const char* fmt, ...) {
    if (!s) return;
    // Shift oldest out if at capacity
    if (s->action_log.count == ACTION_LOG_MAX) {
        for (i32 i = 0; i < ACTION_LOG_MAX - 1; ++i)
            memcpy(s->action_log.entries[i], s->action_log.entries[i + 1], 128);
        s->action_log.count = ACTION_LOG_MAX - 1;
    }
    i32 slot = s->action_log.count;
    if (slot < 0) slot = 0;
    if (slot >= ACTION_LOG_MAX) slot = ACTION_LOG_MAX - 1;
    va_list args;
    va_start(args, fmt);
    vsnprintf(s->action_log.entries[slot], 128, fmt, args);
    va_end(args);
    s->action_log.entries[slot][127] = '\0';
    s->action_log.count = slot + 1;
    s->action_log.inactivity_timer = 0.0f;
}

// build_action_log_panel (the presentation half) now lives in ui/action_log_panel.cpp.
