#include "nav.h"

#include <stdlib.h> // abs

using namespace bs_math;

// 4-connected neighbor offsets (E, W, N, S in tile space; N/S are +/-row).
static const i32 NAV_DC[4] = { 1, -1, 0, 0 };
static const i32 NAV_DR[4] = { 0, 0, 1, -1 };

// Manhattan distance heuristic — admissible & consistent for 4-connected unit-cost grids.
static i32 nav_heuristic(i32 c0, i32 r0, i32 c1, i32 r1) {
    return abs(c0 - c1) + abs(r0 - r1);
}

b8 nav_find_path(const Ship* ship,
                 i32 start_col, i32 start_row,
                 i32 goal_col,  i32 goal_row,
                 Vec2* out_path, i32* out_len) {
    if (!ship || !out_path || !out_len) return FALSE;

    const i32 cols = ship->cols;
    const i32 rows = ship->rows;
    const i32 n    = cols * rows;
    if (n <= 0 || n > SHIP_MAX_TILES) return FALSE;

    // Endpoints must be navigable tiles (rejects walls, hull, glass, empty/out-of-range).
    if (!ship_tile_is_walkable(ship, start_col, start_row)) return FALSE;
    if (!ship_tile_is_walkable(ship, goal_col,  goal_row))  return FALSE;

    // Per-tile A* bookkeeping. static (not stack) to keep these ~57KB of arrays off the
    // stack; single-threaded and fully re-initialized below, so persistence is harmless.
    static i32 came[SHIP_MAX_TILES];   // predecessor tile index on the best path, or -1
    static f32 gscore[SHIP_MAX_TILES]; // cost from start
    static f32 fscore[SHIP_MAX_TILES]; // gscore + heuristic
    static b8  in_open[SHIP_MAX_TILES];
    static b8  in_closed[SHIP_MAX_TILES];

    const f32 INF = 1.0e30f;
    for (i32 i = 0; i < n; ++i) {
        came[i]      = -1;
        gscore[i]    = INF;
        fscore[i]    = INF;
        in_open[i]   = FALSE;
        in_closed[i] = FALSE;
    }

    const i32 start_i = start_row * cols + start_col;
    const i32 goal_i  = goal_row  * cols + goal_col;

    gscore[start_i] = 0.0f;
    fscore[start_i] = (f32)nav_heuristic(start_col, start_row, goal_col, goal_row);
    in_open[start_i] = TRUE;

    // A*: pull the lowest-f open node (linear scan — trivially cheap for <=4096 tiles, and
    // simpler/more robust than a heap for the prototype), expand its 4 neighbors, repeat.
    while (TRUE) {
        i32 current = -1;
        f32 best_f  = INF;
        for (i32 i = 0; i < n; ++i) {
            if (in_open[i] && fscore[i] < best_f) {
                best_f  = fscore[i];
                current = i;
            }
        }
        if (current < 0) return FALSE;        // open set empty -> no path exists
        if (current == goal_i) break;         // reached the goal -> reconstruct

        in_open[current]   = FALSE;
        in_closed[current] = TRUE;

        const i32 cc = current % cols;
        const i32 cr = current / cols;

        for (i32 k = 0; k < 4; ++k) {
            const i32 nc = cc + NAV_DC[k];
            const i32 nr = cr + NAV_DR[k];
            if (nc < 0 || nr < 0 || nc >= cols || nr >= rows) continue;
            if (!ship_tile_is_walkable(ship, nc, nr))         continue;

            const i32 ni = nr * cols + nc;
            if (in_closed[ni]) continue;

            const f32 tentative = gscore[current] + 1.0f; // unit step cost
            if (tentative < gscore[ni]) {
                came[ni]   = current;
                gscore[ni] = tentative;
                fscore[ni] = tentative + (f32)nav_heuristic(nc, nr, goal_col, goal_row);
                in_open[ni] = TRUE;
            }
        }
    }

    // Walk predecessors goal -> start into a scratch chain, then emit reversed (start -> goal)
    // as ship-local tile centers. The chain can't exceed the tile count.
    static i32 chain[SHIP_MAX_TILES];
    i32 count = 0;
    for (i32 t = goal_i; t != -1; t = came[t]) {
        if (count >= SHIP_MAX_TILES) return FALSE; // paranoia: malformed predecessor cycle
        chain[count++] = t;
    }
    if (count > NAV_MAX_PATH) return FALSE; // path longer than the crew's buffer

    for (i32 i = 0; i < count; ++i) {
        const i32 t = chain[count - 1 - i]; // reverse: start first
        const i32 c = t % cols;
        const i32 r = t / cols;
        out_path[i] = ship_tile_center_local(ship, c, r);
    }
    *out_len = count;
    return TRUE;
}
