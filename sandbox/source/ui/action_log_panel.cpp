#include "ui/action_log_panel.h"
#include "game.h"
#include <renderer/bs_ui.h> // bs_ui_* HUD panel API

#define ACTION_LOG_FADE_AFTER 3.0f // seconds of inactivity before fading begins
#define ACTION_LOG_FADE_OVER  2.0f // seconds to lerp from full opacity to idle

// Action Log Panel -- bottom-right HUD. Shows the last 3 messages when idle (fading to
// 15% opacity after 3s inactivity), expands to full 30-entry scrollable history on hover.
// Uses the Consola font via bs_ui_begin_hud_panel. (Presentation half of the action log;
// the data buffer + action_log_push live in sim/action_log.cpp.)
void build_action_log_panel(game_state* s, f32 dt) {
    // Update timer even if panel isn't hovered; fade is a global state.
    s->action_log.inactivity_timer += dt;
    f32 alpha = 1.0f;
    if (s->action_log.inactivity_timer > ACTION_LOG_FADE_AFTER) {
        f32 t = (s->action_log.inactivity_timer - ACTION_LOG_FADE_AFTER) / ACTION_LOG_FADE_OVER;
        if (t > 1.0f) t = 1.0f;
        alpha = 1.0f - t * 0.85f; // fade to 0.15 (15%)
    }
    b8 hovered = FALSE;
    // Set background alpha before opening the panel so the whole window fades together.
    bs_ui_push_alpha(alpha);
    if (bs_ui_begin_hud_panel("ACTION LOG", BS_UI_ANCHOR_BOTTOM_RIGHT, 12.0f)) {
        hovered = bs_ui_is_window_hovered();
        if (hovered) {
            alpha = 1.0f;
            s->action_log.inactivity_timer = 0.0f;
        }
        i32 visible = 3; // collapsed: show only newest 3
        if (hovered)
            visible = s->action_log.count; // expanded: show all
        const f32 TEXT_COL[4] = { 0.86f, 0.90f, 0.96f, 1.00f };
        // Draw newest entries at the bottom (natural log order: chronological).
        i32 start = s->action_log.count - visible;
        if (start < 0) start = 0;
        for (i32 i = start; i < s->action_log.count; ++i) {
            bs_ui_text_colored(TEXT_COL[0], TEXT_COL[1], TEXT_COL[2],
                               TEXT_COL[3] * alpha, s->action_log.entries[i]);
        }
    }
    bs_ui_end_hud_panel();
    bs_ui_pop_alpha(); // Alpha
}
