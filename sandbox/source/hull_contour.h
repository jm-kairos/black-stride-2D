#pragma once

#include <defines.h>
#include <math/math_utils.h>          // bs_math::Vec2
#include <containers/vector.h>        // Vector(T) == std::vector<T>
#include "ship.h"                     // Ship, ship_tile_is_structure

// =====================================================================================
// Hull-contour smoothing in ONE pass (Marching Squares).
//
// Folds what used to be three stages — boundary extraction, staircase simplification, and
// corner rounding — into a single marching-squares walk over the ship's binary tile-occupancy
// grid (predicate ship_tile_is_structure, i.e. tile != TILE_EMPTY). Marching squares traces the
// MIDPOINT (dual) contour: vertices sit on tile-edge midpoints, not tile corners, so every
// convex corner is automatically chamfered to 45 degrees and every concave corner filled — a
// de-blocked silhouette comes straight out of the extractor. There is no separate
// Douglas-Peucker / Chaikin stage; the half-tile chamfer IS the smoothing (fixed amount, not
// tunable — that was the deliberate trade for collapsing three passes into one).
//
// Output is ordered, closed, correctly-wound loops of CORNER vertices in SHIP-LOCAL space (the
// authoring frame of ship_tile_center_local / ship_obb — origin & angle NOT applied), so it is
// pose-INDEPENDENT: computed ONCE per ship (or per map edit), then transformed to world at draw
// time exactly like draw_tile_span (ship_local_to_world + s.rotation = ship->angle). It is
// PURELY COSMETIC — collision / docking / nav keep running on the discrete tile grid.
//
// Algorithm:
//   1. Sample occupancy at tile centers; pad with empty so the contour always closes. Each 2x2
//      sample neighbourhood (a "ms-cell", indexed gx in [-1,cols-1], gy in [-1,rows-1]) yields a
//      4-bit case from its corner occupancies.
//   2. A 16-entry oriented table emits 0/1/2 directed unit segments between edge-midpoints, with
//      the OCCUPIED region kept on the LEFT of travel. Saddle cases (5,10 — occupied on one
//      diagonal) are resolved by SEPARATING the occupied corners (two diagonally-touching cells
//      stay two loops), a fixed decider matching the old crack-follower's behaviour.
//   3. Stitch the directed segments head-to-tail (each midpoint has in-degree == out-degree == 1)
//      into closed loops, then drop collinear interior vertices so only real turns remain.
//
// Winding contract (y-up local space): outer loops CCW => positive signed area; holes CW =>
// negative. NOTE the area is NO LONGER occupied*ts^2 — chamfering removes ts^2/8 at each convex
// corner and adds it at each concave one. The exact invariant is:
//      sum(signed_area) = occupied_tiles*ts^2 - chi*ts^2/2          (chi = Euler char = comps-holes)
// or, summed per lattice corner: -ts^2/8 where exactly 1 of 4 surrounding cells is occupied,
// +ts^2/8 where exactly 3 are, -ts^2/4 at a separated saddle (2 occupied on a diagonal).
// =====================================================================================

// One ship's smoothed boundary: all loop vertices concatenated in `verts` (ship-LOCAL space),
// loop i occupying verts[loop_start[i] .. loop_start[i]+loop_len[i]-1] in traversal order (each
// loop implicitly closed). loop_start.size() == loop_len.size() == number of loops.
struct HullContour {
    Vector(bs_math::Vec2) verts;
    Vector(i32)           loop_start;
    Vector(i32)           loop_len;
};

// Extract the smoothed hull boundary of `ship` into `out` (cleared first). Returns FALSE (and
// leaves `out` empty) if `ship`/`out` is null or the ship has no occupied tiles. Output is in
// ship-LOCAL space and independent of the ship's pose (origin/angle).
b8 ship_hull_contour(const Ship* ship, HullContour* out);

// Shoelace signed area of loop `loop` in `c`, ship-LOCAL (world units^2). Positive => CCW (outer
// boundary); negative => CW (hole). Returns 0 for an out-of-range index or a degenerate loop.
f32 hull_loop_signed_area(const HullContour* c, i32 loop);
