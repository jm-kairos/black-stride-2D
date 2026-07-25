#include "render/debug_overlay.h"
#include "game.h"
#include "core/view_transform.h" // render_from_hierpos
#include "text.h"
#include "core/render_layers.h" // LAYER_UI / LAYER_HUD_TEXT
#include <renderer/renderer.h>
#include <renderer/camera2d.h>
#include <stdio.h> // snprintf

// DEBUG cell-grid overlay toggle (set by the editor panel, read by game_render). Off by default.
b8 g_debug_cell_grid = FALSE;

// DEBUG: HierPos2 cell-grid overlay.
// Visualizes the hierarchical coordinate lattice the simulation lives on: draws the
// BS_HIERPOS_CELL_SIZE cell boundaries around the flagship, a faint parity checkerboard,
// per-cell (cx,cy) labels, and a HUD readout of the flagship/camera cell + local. Every
// line is built from HierPos2 corners routed through render_from_hierpos (the exact path
// entities use), so a broken floating-origin rebase would make the grid shear or jitter
// against the ships. Off by default; toggled from the COORDINATE SPACE editor panel.
void draw_hierpos_cell_grid(const game_state* s) {
    using namespace bs_math;
    if (!s) return;

    const f32 H = BS_HIERPOS_HALF_CELL;   // 8192: cell corner local extent
    const i32 R = 3;                       // cells drawn each direction around the flagship
    const HierPos2& flag = s->player_ship().origin;
    const i64 fcx = flag.cell.x, fcy = flag.cell.y;

    const bs_color grid_col  = bs_color{ 0.30f, 0.70f, 0.95f, 0.55f };
    const bs_color hi_col    = bs_color{ 0.35f, 1.00f, 0.55f, 0.95f }; // current cell
    const bs_color fill_col  = bs_color{ 0.30f, 0.55f, 0.95f, 0.06f }; // parity checker
    const bs_color label_col = bs_color{ 0.70f, 0.85f, 1.00f, 0.75f };

    auto corner = [](i64 cx, i64 cy, f32 lx, f32 ly) {
        HierPos2 p; p.cell = GridCell{ cx, cy }; p.local = Vec2{ lx, ly }; return p;
    };

    // A cell is "active" when ANY fleet ship currently occupies it. Each ship owns an
    // independent HierPos2, so two ships in different cells light up both cells.
    auto cell_occupied = [s](i64 cx, i64 cy) -> b8 {
        for (i32 i = 0; i < s->fleet_state.fleet.count(); ++i) {
            const GridCell& c = s->fleet_state.fleet.at(i).ship.origin.cell;
            if (c.x == cx && c.y == cy) return TRUE;
        }
        return FALSE;
    };

    for (i64 cy = fcy - R; cy <= fcy + R; ++cy) {
        for (i64 cx = fcx - R; cx <= fcx + R; ++cx) {
            HierPos2 cbl = corner(cx, cy, -H, -H);
            HierPos2 cbr = corner(cx, cy,  H, -H);
            HierPos2 ctr = corner(cx, cy,  H,  H);
            HierPos2 ctl = corner(cx, cy, -H,  H);
            Vec2 bl = render_from_hierpos(s, &cbl);
            Vec2 br = render_from_hierpos(s, &cbr);
            Vec2 tr = render_from_hierpos(s, &ctr);
            Vec2 tl = render_from_hierpos(s, &ctl);

            // Parity checkerboard fill (low layer so it sits behind the ships).
            if (((cx + cy) & 1) != 0) {
                Vec2 center = vec2_scale(vec2_add(bl, tr), 0.5f);
                f32 sx = tr.x - bl.x; if (sx < 0.0f) sx = -sx;
                f32 sy = tr.y - bl.y; if (sy < 0.0f) sy = -sy;
                renderer_draw_quad(center, Vec2{ sx, sy }, fill_col, 1);
            }

            b8 cur = cell_occupied(cx, cy);
            const bs_color lc = cur ? hi_col : grid_col;
            f32 th = cur ? 2.5f : 1.0f;
            renderer_draw_line(bl, br, th, lc, LAYER_UI);
            renderer_draw_line(br, tr, th, lc, LAYER_UI);
            renderer_draw_line(tr, tl, th, lc, LAYER_UI);
            renderer_draw_line(tl, bl, th, lc, LAYER_UI);

            // Per-cell coordinate label at the cell center.
            HierPos2 cc = corner(cx, cy, 0.0f, 0.0f);
            Vec2 rc = render_from_hierpos(s, &cc);
            Vec2 sc = camera2d_world_to_screen(&s->camera_state.camera, s->fb_width, s->fb_height, rc);
            char buf[32];
            snprintf(buf, sizeof(buf), "(%lld,%lld)", (long long)cx, (long long)cy);
            text_draw(buf, sc.x - text_width(buf, 1.0f) * 0.5f, sc.y - 4.0f, 1.0f,
                      cur ? hi_col : label_col, &s->camera_state.camera, s->fb_width, s->fb_height, LAYER_HUD_TEXT);
        }
    }

    // The screen-anchored HierPos2 readout (flag/cam cell + local) is now an RmlUi HUD document
    // (see #debug in hud.rml, filled by game_push_hud while g_debug_cell_grid is on). Only the
    // world-anchored grid lines + per-cell labels above remain renderer-drawn.
}
