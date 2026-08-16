#pragma once
#include <defines.h>
#include <math/bs_hierpos.h>
// =====================================================================================
// Galaxy spatial grid — uniform bucket grid over the galaxy disc for O(1)-ish nearest and
// radius queries across ~10,000 star-system nodes. Replaces the O(N) Voronoi nearest-site
// scan and the fixed 64-site cap.
// =====================================================================================
// Built ONCE at galaxy generation and never mutated afterwards, so all query functions are
// pure reads (safe to call lock-free from any thread).
//
// Storage is a counting-sorted CSR: cell_start[c]..cell_start[c+1] indexes node_order[] to
// list the node indices bucketed into grid cell c.

struct GalaxyNode; // full definition in state/game_state.h

struct GalaxySpatialGrid {
    f64  origin_x;    // world-space min corner (with margin)
    f64  origin_y;
    f64  cell_size;   // world units per grid cell
    i32  nx;          // grid width in cells
    i32  ny;          // grid height in cells
    i32* cell_start;  // (nx*ny + 1) CSR prefix offsets into node_order
    i32* node_order;  // node_count node indices, bucketed by cell
    i32  node_count;
};

// Build the grid from the node array (positions read via node.galaxy_center). Allocates the
// CSR arrays from MEMORY_TAG_GAME. `cell_size` is the target world-units bucket edge.
void galaxy_grid_build(GalaxySpatialGrid* grid, const GalaxyNode* nodes, i32 node_count,
                       f64 cell_size);

// Free the CSR arrays (safe on a zero-initialised grid).
void galaxy_grid_free(GalaxySpatialGrid* grid);

// Nearest node to world position (wx, wy). Returns node index or -1 if the grid is empty.
i32 galaxy_grid_nearest(const GalaxySpatialGrid* grid, const GalaxyNode* nodes,
                        f64 wx, f64 wy);

// Gather node indices whose position lies within `radius` of (wx, wy). Writes up to `max`
// indices into out_indices and returns the count written (candidates beyond max are dropped).
i32 galaxy_grid_query_radius(const GalaxySpatialGrid* grid, const GalaxyNode* nodes,
                             f64 wx, f64 wy, f64 radius,
                             i32* out_indices, i32 max);
