#include "sim/galaxy_spatial.h"
#include "game.h"                 // GalaxyNode full definition
#include <core/memory/bs_memory.h>
#include <math.h>

using namespace bs_math;

static inline void node_world(const GalaxyNode* n, f64* x, f64* y) {
    hierpos_to_f64(&n->galaxy_center, BS_HIERPOS_CELL_SIZE, x, y);
}

static inline i32 clampi(i32 v, i32 lo, i32 hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

void galaxy_grid_build(GalaxySpatialGrid* grid, const GalaxyNode* nodes, i32 node_count,
                       f64 cell_size) {
    *grid = GalaxySpatialGrid{};
    grid->node_count = node_count;
    if (node_count <= 0 || cell_size <= 0.0) return;

    // ---- 1. AABB of node positions ----
    f64 min_x = 1e300, min_y = 1e300, max_x = -1e300, max_y = -1e300;
    for (i32 i = 0; i < node_count; ++i) {
        f64 x, y; node_world(&nodes[i], &x, &y);
        if (x < min_x) min_x = x; if (x > max_x) max_x = x;
        if (y < min_y) min_y = y; if (y > max_y) max_y = y;
    }

    grid->cell_size = cell_size;
    grid->origin_x  = min_x - cell_size;   // one-cell margin so edge nodes stay in-bounds
    grid->origin_y  = min_y - cell_size;
    grid->nx = (i32)((max_x - grid->origin_x) / cell_size) + 2;
    grid->ny = (i32)((max_y - grid->origin_y) / cell_size) + 2;
    if (grid->nx < 1) grid->nx = 1;
    if (grid->ny < 1) grid->ny = 1;

    i64 cell_count = (i64)grid->nx * (i64)grid->ny;
    grid->cell_start = (i32*)bs_memory_allocator(sizeof(i32) * (cell_count + 1), MEMORY_TAG_GAME);
    grid->node_order = (i32*)bs_memory_allocator(sizeof(i32) * node_count, MEMORY_TAG_GAME);
    for (i64 c = 0; c <= cell_count; ++c) grid->cell_start[c] = 0;

    // ---- 2. Counting sort: count per cell, prefix-sum, scatter ----
    for (i32 i = 0; i < node_count; ++i) {
        f64 x, y; node_world(&nodes[i], &x, &y);
        i32 cx = clampi((i32)((x - grid->origin_x) / cell_size), 0, grid->nx - 1);
        i32 cy = clampi((i32)((y - grid->origin_y) / cell_size), 0, grid->ny - 1);
        grid->cell_start[(i64)cy * grid->nx + cx + 1]++;
    }
    for (i64 c = 0; c < cell_count; ++c) grid->cell_start[c + 1] += grid->cell_start[c];

    // Temp write cursors (copy of the prefix offsets).
    i32* cursor = (i32*)bs_memory_allocator(sizeof(i32) * cell_count, MEMORY_TAG_GAME);
    for (i64 c = 0; c < cell_count; ++c) cursor[c] = grid->cell_start[c];
    for (i32 i = 0; i < node_count; ++i) {
        f64 x, y; node_world(&nodes[i], &x, &y);
        i32 cx = clampi((i32)((x - grid->origin_x) / cell_size), 0, grid->nx - 1);
        i32 cy = clampi((i32)((y - grid->origin_y) / cell_size), 0, grid->ny - 1);
        i64 c = (i64)cy * grid->nx + cx;
        grid->node_order[cursor[c]++] = i;
    }
    bs_memory_free(cursor, sizeof(i32) * cell_count, MEMORY_TAG_GAME);
}

void galaxy_grid_free(GalaxySpatialGrid* grid) {
    if (!grid) return;
    if (grid->cell_start) {
        i64 cell_count = (i64)grid->nx * (i64)grid->ny;
        bs_memory_free(grid->cell_start, sizeof(i32) * (cell_count + 1), MEMORY_TAG_GAME);
    }
    if (grid->node_order)
        bs_memory_free(grid->node_order, sizeof(i32) * grid->node_count, MEMORY_TAG_GAME);
    *grid = GalaxySpatialGrid{};
}

i32 galaxy_grid_nearest(const GalaxySpatialGrid* grid, const GalaxyNode* nodes,
                        f64 wx, f64 wy) {
    if (!grid || grid->node_count <= 0 || !grid->cell_start) return -1;
    i32 qcx = clampi((i32)((wx - grid->origin_x) / grid->cell_size), 0, grid->nx - 1);
    i32 qcy = clampi((i32)((wy - grid->origin_y) / grid->cell_size), 0, grid->ny - 1);

    i32 best = -1;
    f64 best_d = 1e300;
    i32 max_ring = grid->nx > grid->ny ? grid->nx : grid->ny;

    for (i32 r = 0; r <= max_ring; ++r) {
        // Once a candidate is found, cells in ring r cannot beat it if the ring's inner edge is
        // already farther than the best distance found so far.
        if (best >= 0 && (f64)(r - 1) * grid->cell_size > sqrt(best_d)) break;

        i32 x0 = clampi(qcx - r, 0, grid->nx - 1);
        i32 x1 = clampi(qcx + r, 0, grid->nx - 1);
        i32 y0 = clampi(qcy - r, 0, grid->ny - 1);
        i32 y1 = clampi(qcy + r, 0, grid->ny - 1);
        for (i32 cy = y0; cy <= y1; ++cy) {
            b8 edge_row = (cy == qcy - r) || (cy == qcy + r);
            for (i32 cx = x0; cx <= x1; ++cx) {
                // Only scan the perimeter of the ring (interior handled by earlier rings).
                if (r > 0 && !edge_row && cx != qcx - r && cx != qcx + r) continue;
                i64 c = (i64)cy * grid->nx + cx;
                for (i32 k = grid->cell_start[c]; k < grid->cell_start[c + 1]; ++k) {
                    i32 ni = grid->node_order[k];
                    f64 nx, ny; node_world(&nodes[ni], &nx, &ny);
                    f64 dx = nx - wx, dy = ny - wy;
                    f64 d = dx * dx + dy * dy;
                    if (d < best_d) { best_d = d; best = ni; }
                }
            }
        }
    }
    return best;
}

i32 galaxy_grid_query_radius(const GalaxySpatialGrid* grid, const GalaxyNode* nodes,
                             f64 wx, f64 wy, f64 radius,
                             i32* out_indices, i32 max) {
    if (!grid || grid->node_count <= 0 || !grid->cell_start || max <= 0) return 0;
    f64 r2 = radius * radius;
    i32 cx0 = clampi((i32)((wx - radius - grid->origin_x) / grid->cell_size), 0, grid->nx - 1);
    i32 cx1 = clampi((i32)((wx + radius - grid->origin_x) / grid->cell_size), 0, grid->nx - 1);
    i32 cy0 = clampi((i32)((wy - radius - grid->origin_y) / grid->cell_size), 0, grid->ny - 1);
    i32 cy1 = clampi((i32)((wy + radius - grid->origin_y) / grid->cell_size), 0, grid->ny - 1);

    i32 count = 0;
    for (i32 cy = cy0; cy <= cy1; ++cy) {
        for (i32 cx = cx0; cx <= cx1; ++cx) {
            i64 c = (i64)cy * grid->nx + cx;
            for (i32 k = grid->cell_start[c]; k < grid->cell_start[c + 1]; ++k) {
                i32 ni = grid->node_order[k];
                f64 nx, ny; node_world(&nodes[ni], &nx, &ny);
                f64 dx = nx - wx, dy = ny - wy;
                if (dx * dx + dy * dy <= r2) {
                    if (count < max) out_indices[count] = ni;
                    ++count;
                    if (count >= max) return max;
                }
            }
        }
    }
    return count;
}
