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

// As nav_find_path, but additionally treats a caller-supplied set of tiles as IMPASSABLE for
// THIS search only — used for multi-agent crew avoidance: pass the tiles other crew members
// occupy / have reserved and the returned path routes AROUND them, so two crew never plan onto
// the same tile or through each other. The blocked set is given as parallel (col,row) arrays of
// length `blocked_count` (pass count 0 / null for no dynamic obstacles — identical to
// nav_find_path). The agent's own START tile is never blocked (it may always step off where it
// stands); a blocked GOAL makes the search FAIL (the destination is taken), which the caller
// treats as "wait / pick another tile". Same SUCCESS/FAILURE contract and output format as
// nav_find_path (writes ship-local waypoint centers start->goal, sets *out_len; untouched on
// failure). nav_find_path is implemented as this with an empty obstacle set.
b8 nav_find_path_avoiding(const Ship* ship,
                          i32 start_col, i32 start_row,
                          i32 goal_col,  i32 goal_row,
                          const i32* blocked_cols, const i32* blocked_rows, i32 blocked_count,
                          bs_math::Vec2* out_path, i32* out_len);

// ---- Merged navmesh across a docked pair ---------------------------------------------
// "Connect doors -> merge nav meshes." When two ships dock, their airlocks mate and the doors open,
// so the two tile grids become ONE traversable space joined at the seam. This runs a SINGLE A* over
// the UNION of both ships' grids:
//   * within each hull, the normal 4-connected walkable adjacency;
//   * the mated HULL_DOOR pair (located via ships_docked) is treated as walkable — the doors OPEN —
//     so each door links to its own interior deck tile (already 4-adjacent in-grid); and
//   * a synthetic CONNECTOR bridge node — the walkable tile spawned in the two-tile airlock gap, at
//     the MIDPOINT of the two mated door centers — links the two doors: the cross-hull route is
//     door_a -> connector -> door_b (never a direct door-to-door edge), so the path walks THROUGH the
//     connector tile, matching the rendered/spawned bridge (dock_connector_tile) and the crew glide.
// The result is a continuous route from any tile on either hull to any tile on the other. Endpoints
// are addressed as (ship, col, row) with ship 0 = `sa`, ship 1 = `sb`. The path is emitted in WORLD
// space (out_path_world) — the only frame common to both hulls — as tile centers, ordered start->goal,
// with *out_len set; consecutive waypoints are <= ~1 tile apart (continuous, no jump).
//
// `docked` is the merge latch: pass the game's enemy_docked. When FALSE (or the ships aren't actually
// within `tolerance`) NO doors open and NO connector/seam exists, so the two grids are DISCONNECTED
// components: a SAME-hull query still succeeds, but a CROSS-hull query FAILS — the meshes are SPLIT.
// This is the navigation half of dock/undock: docking merges the meshes, undocking splits them, with
// no separate merged structure to maintain (the union is rebuilt per query from the live poses).
//
// Same SUCCESS/FAILURE contract as nav_find_path: TRUE writes out_path_world/out_len; FALSE (no route,
// bad/blocked endpoint, or path longer than NAV_MAX_PATH) leaves them UNTOUCHED. Both ships must share
// the same tile_size (true for this prototype). Returns FALSE on null args or an empty/oversized grid.
b8 nav_find_path_merged(const Ship* sa, const Ship* sb, b8 docked, f32 tolerance,
                        i32 start_ship, i32 start_col, i32 start_row,
                        i32 goal_ship,  i32 goal_col,  i32 goal_row,
                        bs_math::Vec2* out_path_world, i32* out_len);
