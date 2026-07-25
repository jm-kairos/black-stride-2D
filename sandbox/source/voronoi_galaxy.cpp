#define JC_VORONOI_IMPLEMENTATION
#include "jc_voronoi.h"
#include "voronoi_galaxy.h"
#include "game.h"
#include "core/view_transform.h"
#include <math/math_utils.h>
#include <math/bs_hierpos.h>
#include <renderer/renderer.h>
#include <new>
using namespace bs_math;
// =====================================================================================
// Internal helpers
// =====================================================================================
// point_in_polygon now lives in core/geom2d.cpp (declared via core/geom2d.h, included by game.h).
static void sort_verts_by_angle(Vec2 center, Vec2* verts, i32 count)
{
    if (count < 2) return;
    // 1. Bubble sort by atan2 angle around center (O(n^2), n <= 32)
    for (i32 i = 0; i < count - 1; ++i) {
        for (i32 j = 0; j < count - i - 1; ++j) {
            f32 a0 = atan2f(verts[j].y - center.y, verts[j].x - center.x);
            f32 a1 = atan2f(verts[j+1].y - center.y, verts[j+1].x - center.x);
            if (a0 > a1) {
                Vec2 tmp = verts[j];
                verts[j] = verts[j+1];
                verts[j+1] = tmp;
            }
        }
    }
    // 2. Fix atan2 wraparound: find the largest angular gap and rotate
    //    so the gap is at the array boundary. This ensures consecutive
    //    vertices in the array are actually adjacent on the polygon.
    f32 angles[32];
    for (i32 i = 0; i < count; ++i) {
        angles[i] = atan2f(verts[i].y - center.y, verts[i].x - center.x);
    }
    f32 max_gap = angles[0] - (angles[count - 1] - 2.0f * BS_PI);
    i32 gap_after = count - 1;  // gap between last and first (wrapped)
    for (i32 i = 0; i < count - 1; ++i) {
        f32 gap = angles[i + 1] - angles[i];
        if (gap > max_gap) {
            max_gap = gap;
            gap_after = i;
        }
    }
    // If the largest gap is not at the end, rotate so it is.
    if (gap_after != count - 1) {
        // Rotate left by (gap_after + 1) positions.
        i32 rot = gap_after + 1;
        Vec2 tmp_v[32];
        f32 tmp_a[32];
        for (i32 i = 0; i < count; ++i) {
            tmp_v[i] = verts[i];
            tmp_a[i] = angles[i];
        }
        for (i32 i = 0; i < count; ++i) {
            verts[i] = tmp_v[(i + rot) % count];
            angles[i] = tmp_a[(i + rot) % count];
        }
    }
}
// =====================================================================================
void generate_galaxy_voronoi(const StarSystem* systems, i32 system_count, GalaxyVoronoi* out)
{
    if (!out || system_count <= 0) return;
    memset(out, 0, sizeof(GalaxyVoronoi));
    // ---- 1. Convert star positions to jcv_point array ----
    jcv_point* points = (jcv_point*)malloc(sizeof(jcv_point) * system_count);
    for (i32 i = 0; i < system_count; ++i) {
        f64 x, y;
        hierpos_to_f64(&systems[i].galaxy_center, BS_HIERPOS_CELL_SIZE, &x, &y);
        points[i].x = (jcv_real)x;
        points[i].y = (jcv_real)y;
    }
    // ---- 2. Compute bounding box (expand by 20%) ----
    jcv_rect bbox;
    bbox.min.x = points[0].x;
    bbox.min.y = points[0].y;
    bbox.max.x = points[0].x;
    bbox.max.y = points[0].y;
    for (i32 i = 1; i < system_count; ++i) {
        if (points[i].x < bbox.min.x) bbox.min.x = points[i].x;
        if (points[i].y < bbox.min.y) bbox.min.y = points[i].y;
        if (points[i].x > bbox.max.x) bbox.max.x = points[i].x;
        if (points[i].y > bbox.max.y) bbox.max.y = points[i].y;
    }
    jcv_real pad_x = (bbox.max.x - bbox.min.x) * 0.2f;
    jcv_real pad_y = (bbox.max.y - bbox.min.y) * 0.2f;
    if (pad_x < 1e6f) pad_x = 1e6f;
    if (pad_y < 1e6f) pad_y = 1e6f;
    bbox.min.x -= pad_x;  bbox.min.y -= pad_y;
    bbox.max.x += pad_x;  bbox.max.y += pad_y;
    // ---- 3. Generate Voronoi diagram ----
    jcv_diagram diagram;
    memset(&diagram, 0, sizeof(diagram));
    jcv_diagram_generate(system_count, points, &bbox, nullptr, &diagram);
    // ---- 4. Extract Voronoi edges (internal, excluding borders) ----
    const jcv_edge* edge = jcv_diagram_get_edges(&diagram);
    while (edge) {
        GalaxyVEdge& ve = out->edges[out->edge_count];
        ve.p0 = Vec2{(f32)edge->pos[0].x, (f32)edge->pos[0].y};
        ve.p1 = Vec2{(f32)edge->pos[1].x, (f32)edge->pos[1].y};
        ve.site_0 = edge->sites[0] ? edge->sites[0]->index : -1;
        ve.site_1 = edge->sites[1] ? edge->sites[1]->index : -1;
        ve.is_border = FALSE;
        out->edge_count++;
        edge = jcv_diagram_get_next_edge(edge);
    }
    // ---- 5. Extract Delaunay dual edges ----
    jcv_delauney_iter diter;
    jcv_delauney_edge dedge;
    jcv_delauney_begin(&diagram, &diter);
    while (jcv_delauney_next(&diter, &dedge)) {
        i32 a = dedge.sites[0]->index;
        i32 b = dedge.sites[1]->index;
        // Deduplicate: only store if a < b (each edge appears twice in dual)
        if (a < b) {
            out->delaunay_edges[out->delaunay_edge_count].site_a = a;
            out->delaunay_edges[out->delaunay_edge_count].site_b = b;
            out->delaunay_edge_count++;
        }
    }
    // ---- 6. Build cell polygons from site graphedges ----
    const jcv_site* sites = jcv_diagram_get_sites(&diagram);
    for (i32 i = 0; i < system_count; ++i) {
        const jcv_site* site = &sites[i];
        GalaxyVCell& cell = out->cells[site->index];
        cell.center = Vec2{(f32)site->p.x, (f32)site->p.y};
        cell.vert_count = 0;
        cell.bbox_min = Vec2{ 1e30f, 1e30f };
        cell.bbox_max = Vec2{-1e30f,-1e30f };
        jcv_graphedge* ge = site->edges;
        while (ge) {
            for (i32 ep = 0; ep < 2; ++ep) {
                Vec2 v = Vec2{(f32)ge->pos[ep].x, (f32)ge->pos[ep].y};
                // Deduplicate within cell
                b8 dup = FALSE;
                for (i32 vi = 0; vi < cell.vert_count; ++vi) {
                    f32 dx = cell.verts[vi].x - v.x;
                    f32 dy = cell.verts[vi].y - v.y;
                    if (dx*dx + dy*dy < 100.0f) { dup = TRUE; break; }
                }
                if (!dup && cell.vert_count < 32) {
                    cell.verts[cell.vert_count++] = v;
                    if (v.x < cell.bbox_min.x) cell.bbox_min.x = v.x;
                    if (v.y < cell.bbox_min.y) cell.bbox_min.y = v.y;
                    if (v.x > cell.bbox_max.x) cell.bbox_max.x = v.x;
                    if (v.y > cell.bbox_max.y) cell.bbox_max.y = v.y;
                }
            }
            ge = ge->next;
        }
        sort_verts_by_angle(cell.center, cell.verts, cell.vert_count);
    }
    // ---- 7. Store bbox and site count ----
    out->bbox_min = Vec2{(f32)diagram.min.x, (f32)diagram.min.y};
    out->bbox_max = Vec2{(f32)diagram.max.x, (f32)diagram.max.y};
    out->num_sites = system_count;
    // ---- 8. Cleanup ----
    jcv_diagram_free(&diagram);
    free(points);
}
// =====================================================================================
i32 find_system_by_cell(const HierPos2* pos, const GalaxyVoronoi* v, const StarSystem* systems)
{
    if (!v || v->num_sites <= 0) return -1;
    f64 px, py;
    hierpos_to_f64(pos, BS_HIERPOS_CELL_SIZE, &px, &py);
    // Mathematical definition of Voronoi cell: nearest site by Euclidean distance.
    i32 best = -1;
    f64 best_dist = 1e300;
    for (i32 i = 0; i < v->num_sites; ++i) {
        f64 sx, sy;
        hierpos_to_f64(&systems[i].galaxy_center, BS_HIERPOS_CELL_SIZE, &sx, &sy);
        f64 dx = px - sx;
        f64 dy = py - sy;
        f64 d = dx*dx + dy*dy;
        if (d < best_dist) { best_dist = d; best = i; }
    }
    return best;
}
// =====================================================================================
void draw_voronoi_edges(const GalaxyVoronoi* v, const game_state* s,
                        bs_color edge_col, f32 thickness)
{
    if (!v || !s) return;
    for (i32 i = 0; i < v->edge_count; ++i) {
        const GalaxyVEdge& e = v->edges[i];
        if (e.is_border) continue;
        HierPos2 h0 = hierpos_from_vec2(e.p0, BS_HIERPOS_CELL_SIZE);
        HierPos2 h1 = hierpos_from_vec2(e.p1, BS_HIERPOS_CELL_SIZE);
        Vec2 p0 = hierpos_diff(&h0, &s->camera_state.camera_hierpos, BS_HIERPOS_CELL_SIZE);
        Vec2 p1 = hierpos_diff(&h1, &s->camera_state.camera_hierpos, BS_HIERPOS_CELL_SIZE);
        renderer_draw_line(p0, p1, thickness, edge_col, VORONOI_LAYER_CELESTIAL);
    }
}
// =====================================================================================
void draw_delaunay_lanes(const GalaxyVoronoi* v, const StarSystem* systems,
                         const game_state* s, bs_color lane_col, f32 thickness)
{
    if (!v || !s) return;
    for (i32 i = 0; i < v->delaunay_edge_count; ++i) {
        const GalaxyDEdge& e = v->delaunay_edges[i];
        if (e.site_a < 0 || e.site_b < 0) continue;
        Vec2 pa = hierpos_diff(&systems[e.site_a].galaxy_center, &s->camera_state.camera_hierpos, BS_HIERPOS_CELL_SIZE);
        Vec2 pb = hierpos_diff(&systems[e.site_b].galaxy_center, &s->camera_state.camera_hierpos, BS_HIERPOS_CELL_SIZE);
        renderer_draw_line(pa, pb, thickness, lane_col, VORONOI_LAYER_UI);
    }
}
