#pragma once

#include <defines.h>
#include <math/math_utils.h>
#include "ship.h"

// =====================================================================================
// Ship-interior navigation (prototype): grid A* over the tilemap.
//
// The ship tile grid IS the navmesh. A crew move order runs A* from the crew's current
// tile to a target tile and produces a path the crew steers along. The search and the
// resulting waypoints live in SHIP-LOCAL space, so the path stays valid for free as the
// ship translates and rotates (the crew rides the rigid body — same reason its position
// is stored ship-local). 4-connected (no diagonals): simplest, and fine for the tight
// orthogonal corridors of a tile ship.
// =====================================================================================

// Maximum waypoints a single path may hold. A simple (non-revisiting) grid path can never
// exceed the tile count; this caps it well above the prototype ship's needs and bounds the
// per-crew path buffer (see Crew.path in game.h).
#define NAV_MAX_PATH 512

// Run 4-connected grid A* from (start_col,start_row) to (goal_col,goal_row). "Walkable" =
// ship_tile_is_walkable (floor/door/floor-window); hull, walls, glass, and empty space are
// blocked. Both endpoints must be walkable and in range. On SUCCESS, writes the path as a
// list of ship-LOCAL waypoint CENTERS (ship_tile_center_local), ordered start -> goal
// (out_path[0] = start tile center, out_path[*out_len-1] = goal tile center), sets *out_len,
// and returns TRUE. On FAILURE (no path, blocked/out-of-range endpoint, or a path longer
// than NAV_MAX_PATH) returns FALSE and leaves out_path/out_len UNTOUCHED, so a failed order
// safely keeps the crew's current path.
b8 nav_find_path(const Ship* ship,
                 i32 start_col, i32 start_row,
                 i32 goal_col,  i32 goal_row,
                 bs_math::Vec2* out_path, i32* out_len);
