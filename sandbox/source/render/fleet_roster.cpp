#include "render/fleet_roster.h"

#include "game.h"
#include "render/text.h"           // text_draw / text_width
#include "core/render_layers.h"    // LAYER_UI / LAYER_HUD_TEXT
#include "sim/fleet.h"             // Fleet, FleetStance
#include "sim/action_log.h"        // action_log_push (stance-change feedback)

#include <core/input.h>
#include <renderer/renderer.h>
#include <renderer/camera2d.h>

using namespace bs_math;

// =====================================================================================
// Layout. ONE table, read by both the hit-test and the draw, because a panel whose rows are
// laid out twice will eventually select a different ship from the one under the cursor.
// =====================================================================================

static constexpr f32 ROSTER_X       = 18.0f;   // screen px, left edge
static constexpr f32 ROSTER_Y       = 120.0f;  // screen px, top edge
static constexpr f32 ROSTER_W       = 232.0f;
static constexpr f32 ROW_H          = 30.0f;
static constexpr f32 ROW_GAP        = 3.0f;
static constexpr f32 HEADER_H       = 20.0f;
static constexpr f32 CHIP_W         = 26.0f;
static constexpr f32 CHIP_H         = 13.0f;
static constexpr f32 CHIP_GAP       = 3.0f;
static constexpr f32 TEXT_SCALE     = 1.0f;
static constexpr f32 CHIP_TEXT      = 0.85f;
static constexpr u32 ROSTER_LAYER   = LAYER_UI;
static constexpr u32 ROSTER_TEXT    = LAYER_HUD_TEXT;

// Four stances, four chips. Single-letter labels because a chip that fits a word would make the
// row wider than the ship name it belongs to.
static constexpr i32 STANCE_COUNT = 4;
static const char* const STANCE_CHIP[STANCE_COUNT] = { "A", "D", "P", "H" };
static const char* const STANCE_NAME[STANCE_COUNT] =
    { "AGGRESSIVE", "DEFENSIVE", "PASSIVE", "HOLD FIRE" };

static constexpr bs_color COL_PANEL    = { 0.05f, 0.07f, 0.10f, 0.82f };
static constexpr bs_color COL_ROW      = { 0.10f, 0.13f, 0.17f, 0.85f };
static constexpr bs_color COL_ROW_SEL  = { 0.14f, 0.28f, 0.34f, 0.92f };
static constexpr bs_color COL_EDGE     = { 0.35f, 0.85f, 0.95f, 0.85f };
static constexpr bs_color COL_TEXT     = { 0.82f, 0.90f, 0.95f, 1.0f };
static constexpr bs_color COL_TEXT_DIM = { 0.50f, 0.58f, 0.64f, 1.0f };
static constexpr bs_color COL_CHIP_OFF = { 0.16f, 0.18f, 0.21f, 0.85f };
static constexpr bs_color COL_CHIP_ON  = { 0.85f, 0.62f, 0.25f, 0.95f };
static constexpr bs_color COL_PILOT    = { 0.35f, 1.00f, 0.55f, 1.0f };

static f32 roster_height(i32 n) { return HEADER_H + (f32)n * (ROW_H + ROW_GAP) + ROW_GAP; }

static f32 row_top(i32 i) { return ROSTER_Y + HEADER_H + (f32)i * (ROW_H + ROW_GAP); }

// Chips sit on the row's second line, right-aligned inside the row.
static f32 chip_x(i32 stance) {
    f32 block = (f32)STANCE_COUNT * CHIP_W + (f32)(STANCE_COUNT - 1) * CHIP_GAP;
    f32 x0    = ROSTER_X + ROSTER_W - 6.0f - block;
    return x0 + (f32)stance * (CHIP_W + CHIP_GAP);
}
static f32 chip_y(i32 row) { return row_top(row) + ROW_H - CHIP_H - 4.0f; }

static b8 inside(f32 px, f32 py, f32 x, f32 y, f32 w, f32 h) {
    return (px >= x && px <= x + w && py >= y && py <= y + h) ? TRUE : FALSE;
}

// The roster follows the command overlay: it is a commanding surface, so it appears exactly
// when commanding is what the player is doing. Showing it permanently would put a panel over
// the world during the flying half of the game for no reason.
static b8 roster_visible(const game_state* s) {
    return (s && s->command_overlay_active && s->fleet_state.fleet.count() > 0) ? TRUE : FALSE;
}

b8 fleet_roster_wants_mouse(const game_state* s) {
    if (!roster_visible(s)) return FALSE;
    i32 mx, my;
    input_get_mouse_position(&mx, &my);
    return inside((f32)mx, (f32)my, ROSTER_X, ROSTER_Y, ROSTER_W,
                  roster_height(s->fleet_state.fleet.count()));
}

// =====================================================================================

void fleet_roster_update(game_state* s) {
    if (!roster_visible(s)) return;
    if (!(input_is_button_down(BUTTON_LEFT) && !input_was_button_down(BUTTON_LEFT))) return;

    i32 mx, my;
    input_get_mouse_position(&mx, &my);
    const f32 px = (f32)mx, py = (f32)my;

    Fleet& fleet = s->fleet_state.fleet;
    const b8 additive = input_is_key_down(KEY_LSHIFT) ? TRUE : FALSE;

    for (i32 i = 0; i < fleet.count(); ++i) {
        // Chips first: they sit INSIDE the row, so testing the row first would swallow every
        // chip click and turn a stance change into a selection change.
        for (i32 st = 0; st < STANCE_COUNT; ++st) {
            if (!inside(px, py, chip_x(st), chip_y(i), CHIP_W, CHIP_H)) continue;
            // A chip acts on the whole selection when the clicked ship is part of it, and on
            // that ship alone otherwise. That keeps "set the wing defensive" one click without
            // making a stray click on an unselected row silently retune the whole fleet.
            if (fleet.is_selected(i)) {
                fleet.set_selected_stance((u8)st);
            } else {
                fleet.at(i).stance = (u8)st;
            }
            action_log_push(s, "%s: %s", fleet.at(i).ship.vessel_name, STANCE_NAME[st]);
            return;
        }
        if (!inside(px, py, ROSTER_X, row_top(i), ROSTER_W, ROW_H)) continue;
        if (additive) {
            fleet.set_selected(i, fleet.is_selected(i) ? FALSE : TRUE);
        } else {
            fleet.clear_selection();
            fleet.set_selected(i, TRUE);
        }
        return;
    }
}

// =====================================================================================

static Vec2 rpx(const game_state* s, Vec2 px) {
    return camera2d_screen_to_world(&s->camera_state.camera, s->fb_width, s->fb_height, px);
}

// Screen-space filled rect. renderer_draw_quad works in world units, so the size divides by
// zoom -- the same conversion weapon_hub uses, and correct only while camera.rotation is 0,
// which game.cpp pins.
static void rfill(const game_state* s, f32 x, f32 y, f32 w, f32 h, bs_color col) {
    f32 zoom = (s->camera_state.camera.zoom > 1.0e-9f) ? s->camera_state.camera.zoom : 1.0e-9f;
    renderer_draw_quad(rpx(s, Vec2{ x + w * 0.5f, y + h * 0.5f }),
                       Vec2{ w / zoom, h / zoom }, col, ROSTER_LAYER);
}

static void rtext(const game_state* s, const char* str, f32 x, f32 y, f32 scale, bs_color col) {
    text_draw(str, x, y, scale, col, &s->camera_state.camera, s->fb_width, s->fb_height,
              ROSTER_TEXT);
}

void fleet_roster_draw(const game_state* s) {
    if (!roster_visible(s)) return;
    const Fleet& fleet = s->fleet_state.fleet;
    const i32 n = fleet.count();

    rfill(s, ROSTER_X, ROSTER_Y, ROSTER_W, roster_height(n), COL_PANEL);
    rtext(s, "FLEET", ROSTER_X + 8.0f, ROSTER_Y + 5.0f, TEXT_SCALE, COL_EDGE);

    const i32 piloted = fleet.piloted_index();

    for (i32 i = 0; i < n; ++i) {
        const FleetShip& fs = fleet.at(i);
        const b8 sel = fleet.is_selected(i);
        const f32 ty = row_top(i);

        rfill(s, ROSTER_X + 4.0f, ty, ROSTER_W - 8.0f, ROW_H, sel ? COL_ROW_SEL : COL_ROW);
        // A selected row gets a bright left edge rather than only a fill change: at this panel
        // size the two fills are close enough that a fill alone is easy to miss.
        if (sel) rfill(s, ROSTER_X + 4.0f, ty, 2.0f, ROW_H, COL_EDGE);

        char label[40];
        const char* nm = fs.ship.vessel_name ? fs.ship.vessel_name : "ship";
        snprintf(label, sizeof(label), "%d %.18s", i + 1, nm);
        rtext(s, label, ROSTER_X + 12.0f, ty + 3.0f, TEXT_SCALE,
              (i == piloted) ? COL_PILOT : COL_TEXT);

        // Order state, so the roster answers "what is this ship doing" without a second panel.
        const char* what = "IDLE";
        if      (fs.has_attack_target) what = "ATTACK";
        else if (fs.has_escort_target) what = "ESCORT";
        else if (fs.has_avoid_target)  what = "AVOID";
        else if (fs.has_move_target)   what = "MOVE";
        rtext(s, what, ROSTER_X + 12.0f, ty + ROW_H - 12.0f, CHIP_TEXT, COL_TEXT_DIM);

        for (i32 st = 0; st < STANCE_COUNT; ++st) {
            const b8 on = (fs.stance == (u8)st);
            rfill(s, chip_x(st), chip_y(i), CHIP_W, CHIP_H, on ? COL_CHIP_ON : COL_CHIP_OFF);
            rtext(s, STANCE_CHIP[st], chip_x(st) + CHIP_W * 0.5f - 3.0f, chip_y(i) + 2.0f,
                  CHIP_TEXT, on ? COL_PANEL : COL_TEXT_DIM);
        }
    }
}
