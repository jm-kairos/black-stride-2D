#include "render/voronoi_cell_hover_effect.h"
#include "sim/voronoi_galaxy.h"
#include "game.h"
#include "core/view_transform.h"
#include <math/bs_hierpos.h>
#include <math/math_utils.h>
#include <renderer/renderer.h>
using namespace bs_math;
// =====================================================================================
// Internal helpers
// =====================================================================================
static i32 pick_voronoi_cell(const Vec2& mouse_rel,
                             const HierPos2* camera_hierpos,
                             f32 zoom,
                             const GalaxyVoronoi* v,
                             const StarSystem* systems)
{
    if (!v || v->num_sites <= 0) return -1;
    (void)zoom;
    HierPos2 mouse_abs = hierpos_add_f64(camera_hierpos,
                                          (f64)mouse_rel.x,
                                          (f64)mouse_rel.y,
                                          BS_HIERPOS_CELL_SIZE);
    f64 px, py;
    hierpos_to_f64(&mouse_abs, BS_HIERPOS_CELL_SIZE, &px, &py);
    i32 best = -1;
    f64 best_dist = 1e300;
    for (i32 i = 0; i < v->num_sites; ++i) {
        f64 sx, sy;
        hierpos_to_f64(&systems[i].galaxy_center, BS_HIERPOS_CELL_SIZE, &sx, &sy);
        f64 dx = px - sx;
        f64 dy = py - sy;
        f64 d = dx * dx + dy * dy;
        if (d < best_dist) { best_dist = d; best = i; }
    }
    return best;
}
// =====================================================================================
// Update
// =====================================================================================
void update_cell_hover_effect(GalaxyVoronoi* v, f32 dt,
                              const Vec2& mouse_world,
                              const HierPos2* camera_hierpos,
                              f32 zoom,
                              const StarSystem* systems)
{
    if (!v || !camera_hierpos) return;
    i32 new_hover = pick_voronoi_cell(mouse_world, camera_hierpos, zoom, v, systems);
    if (new_hover != v->hovered_cell) {
        v->hovered_cell = new_hover;
        v->hover_head_dist = 0.0f;
    } else if (v->hovered_cell >= 0) {
        // Accumulate time for the pulse (wrap at 2π to avoid fp drift)
        v->hover_head_dist += dt * BS_PI * 2.0f;
        if (v->hover_head_dist > 100.0f * BS_PI) v->hover_head_dist = 0.0f;
    }
}
// =====================================================================================
// Render
// =====================================================================================
void draw_cell_hover_effect(const GalaxyVoronoi* v,
                            const game_state* s,
                            bs_color base_col)
{
    if (!v || !s || v->hovered_cell < 0) return;
    if (v->hovered_cell >= v->num_sites) return;
    const GalaxyVCell& cell = v->cells[v->hovered_cell];
    if (cell.vert_count < 3) return;
    // Pulse envelope: sin² gives smooth [0,1] oscillation
    f32 t = v->hover_head_dist;
    f32 pulse = sinf(t) * sinf(t);  // 0 → 1 → 0 over ~1 second
    f32 alpha   = 0.3f + 0.7f * pulse;  // 0.3 → 1.0
    f32 thickness = 1.0f + 2.0f * pulse; // 1.0 → 3.0
    bs_color col = base_col;
    col.a *= alpha;
    // Draw each edge of the hovered cell
    for (i32 i = 0; i < cell.vert_count; ++i) {
        Vec2 va = cell.verts[i];
        Vec2 vb = cell.verts[(i + 1) % cell.vert_count];
        HierPos2 h0 = hierpos_from_vec2(va, BS_HIERPOS_CELL_SIZE);
        HierPos2 h1 = hierpos_from_vec2(vb, BS_HIERPOS_CELL_SIZE);
        Vec2 p0 = hierpos_diff(&h0, &s->camera_state.camera_hierpos, BS_HIERPOS_CELL_SIZE);
        Vec2 p1 = hierpos_diff(&h1, &s->camera_state.camera_hierpos, BS_HIERPOS_CELL_SIZE);
        renderer_draw_line(p0, p1, thickness, col, VORONOI_LAYER_CELESTIAL);
    }
}
