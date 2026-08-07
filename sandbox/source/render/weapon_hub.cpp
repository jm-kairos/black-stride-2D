#include "render/weapon_hub.h"
#include "game.h"

#include "core/render_layers.h"   // LAYER_HUD_TEXT (no hand-copied layer constants)
#include "core/cursor_world.h"    // mouse_true_hierpos
#include "render/text.h"          // text_draw / text_width
#include "sim/ship.h"             // ship_nth_mounted_weapon, ship_weapon_fire_state, ...
#include "sim/weapon.h"           // Weapon::name / cooldown_progress
#include "sim/fleet.h"            // FleetShip::attack_target
#include "sim/action_log.h"       // action_log_push

#include <core/input.h>
#include <renderer/renderer.h>
#include <renderer/camera2d.h>
#include <math.h>

using namespace bs_math;

// =====================================================================================
// Layout (SCREEN pixels, origin top-left, +y down)
// =====================================================================================
static const f32 HUB_TILE_W    = 124.0f;
static const f32 HUB_TILE_H    = 46.0f;
static const f32 HUB_GAP       = 8.0f;
static const f32 HUB_MARGIN    = 22.0f;   // clearance between the outer tiles and the viewport edge
                                          // (also leaves room for the overflow hint under South)
static const f32 HUB_DEADZONE  = 26.0f;   // cursor radius that selects nothing (release = cancel)
static const f32 HUB_ARM_PX    = 8.0f;    // cursor travel from the press before anything highlights
static const f32 HUB_REVEAL    = 0.09f;   // seconds for the open animation
static const f32 HUB_NAME_SCALE   = 1.25f;  // weapon-name glyph scale (10 px)
static const f32 HUB_STATUS_SCALE = 1.0f;   // status-word glyph scale (8 px)

// Shapes sit one layer under the labels so the text always wins inside a tile.
static const u32 HUB_LAYER_PANEL = LAYER_HUD_TEXT - 1;
static const u32 HUB_LAYER_TEXT  = LAYER_HUD_TEXT;

// ---- The five hub states, exactly as the player reads them -------------------------------
// WeaponFireState carries more detail than the hub draws: NO_BEARING and STARVED are folded
// into the same faded look as OUT_OF_RANGE (all three mean "armed, but holding fire"), with
// the status word underneath saying which one it is.
enum HubLook : u8 { HUB_READY = 0, HUB_RELOADING, HUB_FADED, HUB_DISABLED };

// Hues only — opacity is NOT baked in here, it comes from hub_look_opacity so that "faded"
// means the whole tile recedes (background, border and both labels together) rather than just
// the outline going translucent over a full-strength panel.
static const bs_color HUB_BG        = bs_color{ 0.04f, 0.06f, 0.10f, 0.82f };
static const bs_color HUB_SELECTED  = bs_color{ 0.35f, 1.00f, 0.55f, 1.0f }; // matches the active-group hull digits
static const bs_color HUB_COL_READY = bs_color{ 0.82f, 0.92f, 1.00f, 1.0f };
static const bs_color HUB_COL_RELOAD= bs_color{ 0.95f, 0.75f, 0.35f, 1.0f };
static const bs_color HUB_COL_FADED = bs_color{ 0.62f, 0.70f, 0.82f, 1.0f };
static const bs_color HUB_COL_DEAD  = bs_color{ 1.00f, 0.30f, 0.28f, 1.0f };

static bs_color hub_look_color(HubLook look) {
    switch (look) {
        case HUB_RELOADING: return HUB_COL_RELOAD;
        case HUB_FADED:     return HUB_COL_FADED;
        case HUB_DISABLED:  return HUB_COL_DEAD;
        default:            return HUB_COL_READY;
    }
}

// Per-state tile opacity. Ready and Reloading are full strength ("bright" / "cooldown overlay");
// Out of Range is the faded one the spec calls for; Disabled sits between the two so the red
// cross stays legible without competing with the live weapons.
static f32 hub_look_opacity(HubLook look) {
    switch (look) {
        case HUB_FADED:    return 0.42f;
        case HUB_DISABLED: return 0.80f;
        default:           return 1.0f;
    }
}

// =====================================================================================
// Geometry — ONE source of truth shared by the hit-test and the draw, so what the player
// clicks is exactly what is lit up.
// =====================================================================================
// The hub blooms around the point the middle button went down, so it lands under the cursor
// instead of making the player's eye travel to a fixed corner of the screen. That also makes it
// read correctly: the frozen aim target IS the press point, so the ring sits on the thing being
// shot at and each tile answers "can this weapon hit *here*".
//
// Clamped so no tile leaves the viewport — a press near an edge slides the whole hub inward
// rather than cropping a slot the player can still flick toward. When the window is too small
// to hold the hub at all, it falls back to screen-centred.
static Vec2 hub_center_px(const game_state* s) {
    const f32 hx  = HUB_TILE_W * 1.5f + HUB_GAP + HUB_MARGIN;   // centre -> outer edge of E/W
    const f32 hy  = HUB_TILE_H * 1.5f + HUB_GAP + HUB_MARGIN;   // centre -> outer edge of N/S
    const f32 fbw = (f32)s->fb_width;
    const f32 fbh = (f32)s->fb_height;

    Vec2 c = s->weapon_hub_press_px;
    c.x = (hx * 2.0f <= fbw) ? clampf(c.x, hx, fbw - hx) : fbw * 0.5f;
    c.y = (hy * 2.0f <= fbh) ? clampf(c.y, hy, fbh - hy) : fbh * 0.5f;
    return c;
}

// Tile center for slot 0=N, 1=E, 2=W, 3=S, relative to the hub center.
static Vec2 hub_slot_px(const game_state* s, i32 slot) {
    Vec2 c = hub_center_px(s);
    switch (slot) {
        case 0: return Vec2{ c.x, c.y - (HUB_TILE_H + HUB_GAP) };   // North
        case 1: return Vec2{ c.x + (HUB_TILE_W + HUB_GAP), c.y };   // East
        case 2: return Vec2{ c.x - (HUB_TILE_W + HUB_GAP), c.y };   // West
        default: return Vec2{ c.x, c.y + (HUB_TILE_H + HUB_GAP) };  // South ("All")
    }
}

// Which slot the cursor points at, by DIRECTION from the hub center (a flick, not a click) —
// so the player never has to land precisely inside a tile. Inside the deadzone: nothing.
static i32 hub_hit_test(const game_state* s) {
    i32 mx = 0, my = 0;
    input_get_mouse_position(&mx, &my);
    Vec2 c = hub_center_px(s);
    f32 dx = (f32)mx - c.x;
    f32 dy = (f32)my - c.y;              // screen y is DOWN
    if (dx * dx + dy * dy < HUB_DEADZONE * HUB_DEADZONE) return -1;
    if (fabsf(dy) >= fabsf(dx)) return (dy < 0.0f) ? 0 : 3;   // North / South
    return (dx > 0.0f) ? 1 : 2;                               // East / West
}

// Hardpoint index behind slot 0..2, or -1 (slot 3 is "All" and owns no hardpoint).
static i32 hub_slot_hardpoint(const Ship* ship, i32 slot) {
    if (!ship || slot < 0 || slot > 2) return -1;
    return ship_nth_mounted_weapon(ship, slot);
}

// =====================================================================================
// Input
// =====================================================================================
void weapon_hub_close(game_state* s) {
    if (!s) return;
    s->weapon_hub_open  = FALSE;
    s->weapon_hub_hover = -1;
}

// The fleet member being flown. Resolved exactly the way game_update resolves it — the RTS
// controls own the index and it is clamped, not rejected — so the hub can never display one
// ship's weapons while the fire path commits to another's.
static FleetShip* hub_piloted(game_state* s) {
    Fleet& fleet = s->fleet_state.fleet;
    if (fleet.count() < 1) return nullptr;
    i32 idx = s->rts_controls.piloted_index();
    if (idx < 0 || idx >= fleet.count()) idx = 0;
    return &fleet.at(idx);
}

// A standing attack order's target, if there is one. This is the only target that may be
// refreshed while the hub is held: it moves under its own power.
static b8 hub_attack_target(game_state* s, HierPos2* out) {
    FleetShip* pf = hub_piloted(s);
    if (!pf || !pf->has_attack_target || !pf->attack_target) return FALSE;
    *out = pf->attack_target->origin;
    return TRUE;
}

void weapon_hub_update(game_state* s, Ship* piloted) {
    if (!s || !piloted) return;

    b8 down = input_is_button_down(BUTTON_MIDDLE);

    if (!s->weapon_hub_open) {
        // Open on the PRESS edge only, so a button already held when a gate opened (e.g. the
        // editor closing mid-drag) does not pop the hub up under the cursor.
        if (down && !input_was_button_down(BUTTON_MIDDLE)) {
            i32 mx = 0, my = 0;
            input_get_mouse_position(&mx, &my);
            s->weapon_hub_open      = TRUE;
            s->weapon_hub_open_time = s->elapsed_time;
            s->weapon_hub_press_px  = Vec2{ (f32)mx, (f32)my };
            s->weapon_hub_hover     = -1;   // nothing chosen until the player actually flicks
            // Freeze what the range readout is judged against. A standing attack order wins;
            // otherwise it is the world point the cursor was over at the instant of the press.
            if (!hub_attack_target(s, &s->weapon_hub_target))
                s->weapon_hub_target = mouse_true_hierpos(s);
        }
        return;
    }

    if (down) {
        // Held: track the flick direction, but only once the cursor has actually left the
        // press point. Without that gate the hub would light up whichever slot the cursor
        // already happened to point at, and a stray middle-click would commit it.
        i32 mx = 0, my = 0;
        input_get_mouse_position(&mx, &my);
        f32 dx = (f32)mx - s->weapon_hub_press_px.x;
        f32 dy = (f32)my - s->weapon_hub_press_px.y;
        if (s->weapon_hub_hover >= 0 || (dx * dx + dy * dy) >= HUB_ARM_PX * HUB_ARM_PX)
            s->weapon_hub_hover = hub_hit_test(s);
        // The FROZEN cursor target is deliberately NOT refreshed -- the cursor is now sweeping
        // the hub itself, and following it would make the range readout swing wildly as the
        // player chooses. Only a live attack order, which moves on its own, updates it.
        hub_attack_target(s, &s->weapon_hub_target);
        return;
    }

    // Released: commit whatever is highlighted. Deadzone (-1) commits nothing.
    i32 slot = s->weapon_hub_hover;
    weapon_hub_close(s);

    if (slot < 0) return;

    if (slot == 3) {
        if (piloted->weapon_override >= 0) {
            ship_clear_weapon_override(piloted);
            i32 n = ship_group_size(piloted, piloted->active_group);
            action_log_push(s, "All weapons - group %d (%d weapon%s).",
                            piloted->active_group + 1, n, n == 1 ? "" : "s");
        }
        return;
    }

    i32 hp = hub_slot_hardpoint(piloted, slot);
    if (hp < 0) return;                       // slot maps to no installed weapon

    ship_select_weapon_override(piloted, hp);
    const Weapon* w = piloted->mounts[hp];
    action_log_push(s, "Weapon selected: %s.", (w && w->name) ? w->name : "unnamed");
}

// =====================================================================================
// Draw
// =====================================================================================
// Screen pixel -> render space. Every point is converted individually so lines stay correct
// under any camera transform (the same trick sensor_overlay.cpp uses for its chevrons).
static Vec2 hub_px(const game_state* s, Vec2 px) {
    return camera2d_screen_to_world(&s->camera_state.camera, s->fb_width, s->fb_height, px);
}

// Filled axis-aligned rectangle given a SCREEN-space center and size. renderer_draw_quad takes
// world units, so the size divides by zoom. Axis-aligned on screen only while the camera is
// unrotated, which game.cpp pins to 0 (`camera.rotation = 0.0f`).
static void hub_fill_px(const game_state* s, Vec2 center_px, Vec2 size_px, bs_color col, u32 layer) {
    f32 zoom = (s->camera_state.camera.zoom > 1.0e-9f) ? s->camera_state.camera.zoom : 1.0e-9f;
    renderer_draw_quad(hub_px(s, center_px), Vec2{ size_px.x / zoom, size_px.y / zoom }, col, layer);
}

// Rectangle outline from four screen-space corners (thickness is already in screen pixels).
static void hub_outline_px(const game_state* s, Vec2 center_px, Vec2 size_px, f32 th, bs_color col, u32 layer) {
    f32 hx = size_px.x * 0.5f, hy = size_px.y * 0.5f;
    Vec2 tl = hub_px(s, Vec2{ center_px.x - hx, center_px.y - hy });
    Vec2 tr = hub_px(s, Vec2{ center_px.x + hx, center_px.y - hy });
    Vec2 br = hub_px(s, Vec2{ center_px.x + hx, center_px.y + hy });
    Vec2 bl = hub_px(s, Vec2{ center_px.x - hx, center_px.y + hy });
    renderer_draw_line(tl, tr, th, col, layer);
    renderer_draw_line(tr, br, th, col, layer);
    renderer_draw_line(br, bl, th, col, layer);
    renderer_draw_line(bl, tl, th, col, layer);
}

static void hub_line_px(const game_state* s, Vec2 a_px, Vec2 b_px, f32 th, bs_color col, u32 layer) {
    renderer_draw_line(hub_px(s, a_px), hub_px(s, b_px), th, col, layer);
}

// Copy `src` into `dst` clipped to whatever fits `max_px` at `scale` (monospace 8px glyphs).
static void hub_fit_label(char* dst, i32 dst_cap, const char* src, f32 max_px, f32 scale) {
    i32 max_chars = (i32)(max_px / (8.0f * scale));
    if (max_chars > dst_cap - 1) max_chars = dst_cap - 1;
    if (max_chars < 1) { dst[0] = '\0'; return; }
    i32 n = 0;
    while (src && src[n] && n < max_chars) { dst[n] = src[n]; ++n; }
    dst[n] = '\0';
}

static void hub_text_centered(const game_state* s, const char* str, f32 cx, f32 y,
                              f32 scale, bs_color col) {
    text_draw(str, cx - text_width(str, scale) * 0.5f, y, scale, col,
              &s->camera_state.camera, s->fb_width, s->fb_height, HUB_LAYER_TEXT);
}

// One slot. `hp` < 0 with slot < 3 means "no weapon installed here".
static void hub_draw_slot(game_state* s, const Ship* ship, i32 slot, i32 hp,
                          Vec2 dir_to_target, f32 dist, f32 reveal) {
    Vec2 c    = hub_slot_px(s, slot);
    Vec2 size = Vec2{ HUB_TILE_W * reveal, HUB_TILE_H * reveal };

    const b8 is_all    = (slot == 3);
    const b8 empty     = (!is_all && hp < 0);
    const b8 hovered   = (s->weapon_hub_hover == slot);
    // "Selected" is the ship's COMMITTED state, not the hover: All is selected when no
    // override is active, a weapon slot when the override points at its hardpoint.
    const b8 selected  = is_all ? (ship->weapon_override < 0)
                                : (hp >= 0 && ship->weapon_override == (i8)hp);

    HubLook     look   = HUB_READY;
    const char* status = "READY";
    f32         cooldown = 0.0f;

    if (empty) {
        look = HUB_DISABLED; status = "NO MOUNT";
    } else if (is_all) {
        i32 n = ship_group_size(ship, ship->active_group);
        look   = (n > 0) ? HUB_READY : HUB_FADED;
        status = (n > 0) ? "FIRE GROUP" : "EMPTY GROUP";
    } else {
        switch (ship_weapon_fire_state(ship, hp, dir_to_target, dist)) {
            case WEAPON_FIRE_READY:        look = HUB_READY;     status = "READY";        break;
            case WEAPON_FIRE_RELOADING:    look = HUB_RELOADING; status = "RELOADING";
                                           cooldown = ship->mounts[hp]->cooldown_progress(); break;
            case WEAPON_FIRE_OUT_OF_RANGE: look = HUB_FADED;     status = "OUT OF RANGE"; break;
            case WEAPON_FIRE_NO_BEARING:   look = HUB_FADED;     status = "NO BEARING";   break;
            case WEAPON_FIRE_STARVED:      look = HUB_FADED;     status = "NO POWER";     break;
            default:                       look = HUB_DISABLED;  status = "DISABLED";     break;
        }
    }

    bs_color accent = hub_look_color(look);

    // One opacity for the whole tile, so "faded" reads as the slot receding rather than just a
    // translucent border on a full-strength panel. `reveal` folds the open animation in.
    const f32 op = hub_look_opacity(look) * reveal;

    // Background, brightened under the cursor so the flick target is unmistakable.
    bs_color bg = HUB_BG;
    if (hovered) { bg.r += 0.10f; bg.g += 0.13f; bg.b += 0.18f; bg.a = 0.92f; }
    bg.a *= op;
    hub_fill_px(s, c, size, bg, HUB_LAYER_PANEL);

    // Reloading: a cooldown bar filling left-to-right across the bottom of the tile.
    if (look == HUB_RELOADING && cooldown > 0.0f) {
        f32 bar_h = 4.0f * reveal;
        f32 w     = size.x * cooldown;
        bs_color bar = HUB_COL_RELOAD; bar.a = 0.55f * op;
        hub_fill_px(s, Vec2{ c.x - (size.x - w) * 0.5f, c.y + size.y * 0.5f - bar_h * 0.5f },
                    Vec2{ w, bar_h }, bar, HUB_LAYER_PANEL);
    }

    // Outline: the committed selection wins over the state colour, and thickens on hover. The
    // selection border keeps a minimum opacity even on a faded tile -- an out-of-range weapon
    // stays selectable, so the player must be able to see that it IS the one selected.
    bs_color edge = selected ? HUB_SELECTED : accent;
    edge.a *= selected ? (reveal * fmaxf(hub_look_opacity(look), 0.9f)) : op;
    hub_outline_px(s, c, size, selected ? 2.5f : (hovered ? 2.0f : 1.2f), edge, HUB_LAYER_PANEL);

    // Disabled: cross the tile out.
    if (look == HUB_DISABLED) {
        f32 hx = size.x * 0.5f - 6.0f, hy = size.y * 0.5f - 6.0f;
        bs_color x = HUB_COL_DEAD; x.a *= op;
        hub_line_px(s, Vec2{ c.x - hx, c.y - hy }, Vec2{ c.x + hx, c.y + hy }, 2.0f, x, HUB_LAYER_PANEL);
        hub_line_px(s, Vec2{ c.x + hx, c.y - hy }, Vec2{ c.x - hx, c.y + hy }, 2.0f, x, HUB_LAYER_PANEL);
    }

    if (reveal < 0.999f) return;   // labels only once the tile has finished opening

    // Name (top line) + status (bottom line), both clipped to the tile.
    const char* name = is_all ? "ALL" : (empty ? "-" : (ship->mounts[hp]->name ? ship->mounts[hp]->name : "WEAPON"));
    char buf[48];
    hub_fit_label(buf, (i32)sizeof(buf), name, HUB_TILE_W - 12.0f, HUB_NAME_SCALE);
    bs_color name_col = selected ? HUB_SELECTED : accent;
    name_col.a *= selected ? fmaxf(hub_look_opacity(look), 0.9f) : hub_look_opacity(look);
    hub_text_centered(s, buf, c.x, c.y - 13.0f, HUB_NAME_SCALE, name_col);

    hub_fit_label(buf, (i32)sizeof(buf), status, HUB_TILE_W - 10.0f, HUB_STATUS_SCALE);
    bs_color st_col = accent; st_col.a *= 0.85f * hub_look_opacity(look);
    hub_text_centered(s, buf, c.x, c.y + 4.0f, HUB_STATUS_SCALE, st_col);
}

void weapon_hub_draw(game_state* s) {
    if (!s || !s->weapon_hub_open) return;

    FleetShip* pf = hub_piloted(s);
    if (!pf) return;
    const Ship* ship = &pf->ship;

    // Reveal animation: tiles scale up from the center over HUB_REVEAL seconds.
    f32 age    = s->elapsed_time - s->weapon_hub_open_time;
    f32 reveal = (HUB_REVEAL > 0.0f) ? (age / HUB_REVEAL) : 1.0f;
    if (reveal < 0.15f) reveal = 0.15f;
    if (reveal > 1.0f)  reveal = 1.0f;

    // Aim geometry for the per-weapon readout, measured from the SHIP (each hardpoint's own
    // fire origin is within a hull length of it, far below any reach the ring shows).
    Vec2 to_target = hierpos_diff(&s->weapon_hub_target, &ship->origin, BS_HIERPOS_CELL_SIZE);
    f32  dist      = vec2_length(to_target);

    Vec2 center = hub_center_px(s);

    // Center hub: a ring the tiles radiate from, plus a pointer toward the highlighted slot.
    f32 zoom = (s->camera_state.camera.zoom > 1.0e-9f) ? s->camera_state.camera.zoom : 1.0e-9f;
    bs_color ring = bs_color{ 0.55f, 0.68f, 0.85f, 0.45f * reveal };
    renderer_draw_circle(hub_px(s, center), (HUB_DEADZONE * reveal) / zoom, 24, 1.5f, ring, HUB_LAYER_PANEL);

    if (s->weapon_hub_hover >= 0) {
        Vec2 slot_c = hub_slot_px(s, s->weapon_hub_hover);
        Vec2 d = Vec2{ slot_c.x - center.x, slot_c.y - center.y };
        f32  dl = sqrtf(d.x * d.x + d.y * d.y);
        // Stop at the tile's near EDGE, which is half a tile back along the axis this slot sits
        // on -- East/West are a half-width away, North/South a half-height, and the two differ
        // enough that using one for both drives the pointer well inside the tile.
        b8  horizontal = (s->weapon_hub_hover == 1 || s->weapon_hub_hover == 2);
        f32 stop = dl - (horizontal ? HUB_TILE_W : HUB_TILE_H) * 0.5f;
        if (dl > 1.0e-3f && stop > HUB_DEADZONE) {
            d = Vec2{ d.x / dl, d.y / dl };
            bs_color p = HUB_SELECTED; p.a = 0.75f * reveal;
            hub_line_px(s, Vec2{ center.x + d.x * HUB_DEADZONE, center.y + d.y * HUB_DEADZONE },
                        Vec2{ center.x + d.x * stop, center.y + d.y * stop },
                        2.0f, p, HUB_LAYER_PANEL);
        }
    }

    for (i32 slot = 0; slot < WEAPON_HUB_SLOTS; ++slot)
        hub_draw_slot(s, ship, slot, hub_slot_hardpoint(ship, slot), to_target, dist, reveal);

    // A hull with more weapons than the three directional slots can hold: say so, rather than
    // leaving the player to wonder where the fourth cannon went.
    if (reveal >= 0.999f && ship_nth_mounted_weapon(ship, 3) >= 0) {
        bs_color hint = bs_color{ 0.70f, 0.78f, 0.90f, 0.55f };
        hub_text_centered(s, "+more via ALL / fire groups 1-5",
                          center.x, center.y + HUB_TILE_H * 1.5f + HUB_GAP + 6.0f,
                          HUB_STATUS_SCALE, hint);
    }
}
