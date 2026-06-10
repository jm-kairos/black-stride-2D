#pragma once

#include "defines.h"

// =====================================================================================
// bs_ui — a tiny immediate-mode widget facade over Dear ImGui. SDL-free AND ImGui-free at the
// header level (mirrors bs_imgui.h): the game builds panels with these calls each frame and the
// implementation (engine/source/renderer/bs_ui.cpp) translates them into ImGui:: calls. ImGui's
// own per-frame NewFrame/Render are sequenced by the GPU backend, so the game never sees them.
//
// The implementation TU includes <imgui.h> (exposed to engine TUs via -isystem) but never SDL or
// the GPU backend — it only drives ImGui's backend-agnostic core, which runs on the single global
// context created by bs_imgui_initialize. That keeps the "only the backend touches SDL/GPU" seam
// intact while letting gameplay code build UI in plain, engine-native types.
//
// Usage (each frame, between renderer_begin_frame and renderer_end_frame, i.e. in game_render):
//
//   if (bs_ui_begin_panel("CREW - JOB CONTROL", BS_UI_ANCHOR_TOP_RIGHT, 12.0f)) {
//       bs_ui_text_colored(0.86f, 0.90f, 0.96f, 1.0f, "Job: Idle");
//       bs_ui_progress(0.5f, "Performing 50%");
//       if (bs_ui_button("Assign Piloting", TRUE)) { /* dispatch action */ }
//   }
//   bs_ui_end_panel();   // ALWAYS call, even when begin returned FALSE (ImGui Begin/End rule)
//
// Input gating: use bs_imgui_wants_mouse() / _keyboard() (bs_imgui.h) to suppress world picking
// while the cursor is over a panel — bs_ui panels are ImGui windows, so those flags already cover
// them (this replaces the old ui_wants_mouse()).
// =====================================================================================

// Which screen corner a panel is pinned to. The panel auto-sizes to its contents and offsets from
// the anchored edge(s) by the `margin` passed to bs_ui_begin_panel. Display size comes from ImGui
// (GetIO().DisplaySize), so no framebuffer dimensions are needed at the call site.
typedef enum BsUiAnchor {
    BS_UI_ANCHOR_TOP_LEFT  = 0,
    BS_UI_ANCHOR_TOP_RIGHT = 1,
} BsUiAnchor;

typedef enum BsUiType {
    BS_UI_TYPE_EDITOR,
    BS_UI_TYPE_GAME
} BsUiType;

// Begin a screen-anchored, auto-sized panel. Returns TRUE if the panel is visible and its body
// should be built this frame. You MUST call bs_ui_end_panel() afterwards regardless of the return
// value (ImGui's Begin/End pairing rule). `margin` is the gap in screen pixels from the anchored
// edge(s). The window is fixed (no move/resize/collapse) and has NO native title bar — panels draw
// their own styled header line, so a native bar would just duplicate it. `title` is still the
// window's ImGui id (it must be unique per panel), it is simply not rendered as a caption.
bs__api__ b8 bs_ui_begin_panel(const char* title, BsUiAnchor anchor, f32 margin, BsUiType ui_type);

// Close the current panel. Pair with every bs_ui_begin_panel call.
bs__api__ void bs_ui_end_panel(void);

// One line of text in the panel's default text color.
bs__api__ void bs_ui_text(const char* text);

// One line of text in an explicit RGBA color (0..1 per channel).
bs__api__ void bs_ui_text_colored(f32 r, f32 g, f32 b, f32 a, const char* text);

// A horizontal progress bar filled to `fraction` (clamped 0..1). `overlay` (may be NULL) is drawn
// centered on the bar, e.g. "Performing 50%".
bs__api__ void bs_ui_progress(f32 fraction, const char* overlay);

// A full-width button. Returns TRUE on the frame it is clicked. A disabled button is dimmed and
// never fires. The button stretches to the panel's content width, but if its own label is wider it
// grows the auto-sized panel to fit instead of clipping the text — so long labels stay readable.
bs__api__ b8 bs_ui_button(const char* label, b8 enabled);

// A fixed-width button (screen px) for compact control clusters like the queue's ^ / v / X row.
// Returns TRUE on the frame it is clicked; disabled buttons are dimmed and inert.
bs__api__ b8 bs_ui_button_sized(const char* label, f32 width, b8 enabled);

// Keep the next widget on the same line as the previous one (for horizontal button clusters).
bs__api__ void bs_ui_same_line(void);

// A faint horizontal separator rule spanning the panel's content width.
bs__api__ void bs_ui_separator(void);

bs__api__ void bs_ui_checkbox(const char* label, bool* enabled);
