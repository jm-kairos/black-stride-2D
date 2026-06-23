#include "voronoi_cell_hover_effect.h"
#include "voronoi_galaxy.h"
#include "game.h"
#include <math/bs_hierpos.h>
#include <math/math_utils.h>
#include <renderer/renderer.h>
using namespace bs_math;
// =====================================================================================
// Internal helpers
// =====================================================================================
// Scalar compression factor (must match game.cpp)
static f32 compression_factor(f32 zoom) {
    const f32 threshold = 0.02f;
    const f32 min_zoom  = 0.000004f;
    if (zoom >= threshold) return 1.0f;
    f32 t = (zoom - min_zoom) / (threshold - min_zoom);
    return 0.15f + 0.85f * t;
}
static Vec2 cosmetic_compress(Vec2 pos, f32 zoom) {
    return vec2_scale(pos, compression_factor(zoom));
}
static i32 pick_voronoi_cell(const Vec2& mouse_rel,
                             const HierPos2* camera_hierpos,
                             f32 zoom,
                             const GalaxyVoronoi* v,
                             const StarSystem* systems)
{
    if (!v || v->num_sites <= 0) return -1;
    f32 comp = compression_factor(zoom);
    Vec2 mouse_uncompressed = (comp > 0.0f)
        ? vec2_scale(mouse_rel, 1.0f / comp)
        : mouse_rel;
    HierPos2 mouse_abs = hierpos_add_f64(camera_hierpos,
                                          (f64)mouse_uncompressed.x,
                                          (f64)mouse_uncompressed.y,
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
        Vec2 p0 = hierpos_diff(&h0, &s->camera_hierpos, BS_HIERPOS_CELL_SIZE);
        Vec2 p1 = hierpos_diff(&h1, &s->camera_hierpos, BS_HIERPOS_CELL_SIZE);
        p0 = cosmetic_compress(p0, s->camera.zoom);
        p1 = cosmetic_compress(p1, s->camera.zoom);
        renderer_draw_line(p0, p1, thickness, col, VORONOI_LAYER_CELESTIAL);
    }
}
