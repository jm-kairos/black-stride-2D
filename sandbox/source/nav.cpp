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

b8 nav_find_path_avoiding(const Ship* ship,
                          i32 start_col, i32 start_row,
                          i32 goal_col,  i32 goal_row,
                          const i32* blocked_cols, const i32* blocked_rows, i32 blocked_count,
                          Vec2* out_path, i32* out_len) {
    if (!ship || !out_path || !out_len) return FALSE;

    const i32 cols = ship->cols;
    const i32 rows = ship->rows;
    const i32 n    = cols * rows;
    if (n <= 0 || n > SHIP_MAX_TILES) return FALSE;

    // Endpoints must be navigable tiles (rejects walls, hull, glass, empty/out-of-range).
    if (!ship_tile_is_walkable(ship, start_col, start_row)) return FALSE;
    if (!ship_tile_is_walkable(ship, goal_col,  goal_row))  return FALSE;

    // Per-tile A* bookkeeping. static (not stack) to keep these ~60KB of arrays off the
    // stack; single-threaded and fully re-initialized below, so persistence is harmless.
    static i32 came[SHIP_MAX_TILES];   // predecessor tile index on the best path, or -1
    static f32 gscore[SHIP_MAX_TILES]; // cost from start
    static f32 fscore[SHIP_MAX_TILES]; // gscore + heuristic
    static b8  in_open[SHIP_MAX_TILES];
    static b8  in_closed[SHIP_MAX_TILES];
    // Dynamic obstacles (OTHER crew's reserved/occupied tiles). Marked impassable for THIS
    // search only, so an agent routes around its peers. Reset per call alongside the rest.
    static b8  dyn_block[SHIP_MAX_TILES];

    const f32 INF = 1.0e30f;
    for (i32 i = 0; i < n; ++i) {
        came[i]      = -1;
        gscore[i]    = INF;
        fscore[i]    = INF;
        in_open[i]   = FALSE;
        in_closed[i] = FALSE;
        dyn_block[i] = FALSE;
    }

    const i32 start_i = start_row * cols + start_col;
    const i32 goal_i  = goal_row  * cols + goal_col;

    // Mark the caller's dynamic obstacle tiles. The START tile is NEVER blocked — an agent may
    // always leave the tile it currently stands on, even if a peer's footprint overlaps it. A
    // blocked GOAL is left blocked on purpose: the search then fails (no agent should be routed
    // onto a tile a peer has reserved), which the caller reads as "destination taken".
    if (blocked_cols && blocked_rows) {
        for (i32 b = 0; b < blocked_count; ++b) {
            const i32 bc = blocked_cols[b];
            const i32 br = blocked_rows[b];
            if (bc < 0 || br < 0 || bc >= cols || br >= rows) continue;
            const i32 bi = br * cols + bc;
            if (bi == start_i) continue; // never wall an agent inside its own start tile
            dyn_block[bi] = TRUE;
        }
    }

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
            if (dyn_block[ni]) continue; // a peer occupies/reserves this tile -> route around it
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

b8 nav_find_path(const Ship* ship,
                 i32 start_col, i32 start_row,
                 i32 goal_col,  i32 goal_row,
                 Vec2* out_path, i32* out_len) {
    // Static-obstacle A* is exactly the dynamic search with an empty obstacle set.
    return nav_find_path_avoiding(ship, start_col, start_row, goal_col, goal_row,
                                  nullptr, nullptr, 0, out_path, out_len);
}

// =====================================================================================
// Merged navmesh across a docked pair (see nav.h). One A* over the UNION of both ships' grids,
// joined at the mated airlock seam when `docked`. Node indexing: ship 0 (sa) occupies indices
// [0, na); ship 1 (sb) occupies [na, na+nb). Costs/heuristic are in TILE units derived from WORLD
// distance (world_dist / tile_size), so the two local frames combine consistently: a 4-adjacent
// in-hull step costs exactly 1 (rotation is rigid, so adjacent tile centers stay tile_size apart),
// and the seam hops cost the real world gaps between the mated doors and the connector. World
// Euclidean distance to the goal (over tile_size) is an admissible+consistent heuristic across frames.
//
// The seam is bridged by a synthetic CONNECTOR node (index N, one past both hulls) — the walkable
// bridge tile spawned in the two-tile gap, sited at the MIDPOINT of the two mated door centers. The
// cross-hull route is door_a <-> connector <-> door_b (never a direct door-to-door edge), so the
// emitted path walks THROUGH the connector tile, matching the rendered bridge and the crew glide.
// =====================================================================================
#define NAV_MERGE_MAX (2 * SHIP_MAX_TILES + 1) // +1 for the synthetic connector bridge node

b8 nav_find_path_merged(const Ship* sa, const Ship* sb, b8 docked, f32 tolerance,
                        i32 start_ship, i32 start_col, i32 start_row,
                        i32 goal_ship,  i32 goal_col,  i32 goal_row,
                        Vec2* out_path_world, i32* out_len) {
    if (!sa || !sb || !out_path_world || !out_len) return FALSE;

    const i32 ca = sa->cols, rA = sa->rows, na = ca * rA;
    const i32 cb = sb->cols, rB = sb->rows, nb = cb * rB;
    if (na <= 0 || nb <= 0 || na > SHIP_MAX_TILES || nb > SHIP_MAX_TILES) return FALSE;
    const i32 N = na + nb;

    const f32 ts = sa->tile_size;
    if (ts <= 0.0f) return FALSE;

    // Resolve a (ship,col,row) address to a flat merged-node index, or -1 if out of range.
    auto node_of = [&](i32 ship, i32 col, i32 row) -> i32 {
        if (ship == 0) { if (col < 0 || row < 0 || col >= ca || row >= rA) return -1; return row * ca + col; }
        else           { if (col < 0 || row < 0 || col >= cb || row >= rB) return -1; return na + row * cb + col; }
    };
    // Decompose a merged node back into (ship, col, row) and its owning Ship*.
    auto split = [&](i32 idx, i32* ship, i32* col, i32* row, const Ship** sp) {
        if (idx < na) { *ship = 0; *col = idx % ca; *row = idx / ca; *sp = sa; }
        else          { i32 j = idx - na; *ship = 1; *col = j % cb; *row = j / cb; *sp = sb; }
    };

    const i32 start_i = node_of(start_ship, start_col, start_row);
    const i32 goal_i  = node_of(goal_ship,  goal_col,  goal_row);
    if (start_i < 0 || goal_i < 0) return FALSE;

    // Locate the mated door pair (the seam). Only when docked AND actually within tolerance do the
    // doors open and the seam edge exist — this is the merge/split latch. da/db are merged-node ids.
    i32 da = -1, db = -1;
    if (docked) {
        i32 dca, dra, dcb, drb;
        if (ships_docked(sa, sb, tolerance, &dca, &dra, &dcb, &drb)) {
            da = node_of(0, dca, dra);
            db = node_of(1, dcb, drb);
        }
    }

    // The seam is bridged by a synthetic CONNECTOR node at index N (one past both hulls), present only
    // when the doors are actually mated. Cross-hull routes go da <-> conn <-> db (never a direct
    // door-to-door edge), so the emitted path walks through the connector bridge tile. NT is the active
    // node count (hull tiles, plus the connector when it exists).
    const i32 conn = (da >= 0 && db >= 0) ? N : -1;
    const i32 NT   = (conn >= 0) ? N + 1 : N;

    // Per-node walkability: normal deck tiles, PLUS the two mated door tiles (doors open on dock).
    static b8  walk[NAV_MERGE_MAX];
    static Vec2 wc[NAV_MERGE_MAX];   // world-space tile centers (the only frame common to both hulls)
    for (i32 i = 0; i < N; ++i) {
        i32 sh, c, r; const Ship* sp;
        split(i, &sh, &c, &r, &sp);
        walk[i] = ship_tile_is_walkable(sp, c, r);
        wc[i]   = ship_tile_center_world(sp, c, r);
    }
    if (da >= 0 && db >= 0) { walk[da] = TRUE; walk[db] = TRUE; } // mated airlocks become passable
    if (conn >= 0) {
        // The connector bridge tile: walkable, sited at the MIDPOINT of the two mated door centers
        // (the one clear tile in the two-tile gap). Same point dock_connector_tile() hands the game
        // loop to spawn+render, so the nav bridge and the rendered/spawned tile agree exactly.
        walk[conn] = TRUE;
        wc[conn]   = vec2_scale(vec2_add(wc[da], wc[db]), 0.5f);
    }

    // Endpoints must be navigable (a non-walkable start/goal can't anchor a route).
    if (!walk[start_i] || !walk[goal_i]) return FALSE;

    static i32 came[NAV_MERGE_MAX];
    static f32 gscore[NAV_MERGE_MAX];
    static f32 fscore[NAV_MERGE_MAX];
    static b8  in_open[NAV_MERGE_MAX];
    static b8  in_closed[NAV_MERGE_MAX];

    const f32 INF = 1.0e30f;
    for (i32 i = 0; i < NT; ++i) {
        came[i] = -1; gscore[i] = INF; fscore[i] = INF; in_open[i] = FALSE; in_closed[i] = FALSE;
    }
    // Admissible heuristic: straight-line world distance to the goal, in tile units.
    auto heur = [&](i32 i) -> f32 { return vec2_length(vec2_sub(wc[goal_i], wc[i])) / ts; };

    gscore[start_i]  = 0.0f;
    fscore[start_i]  = heur(start_i);
    in_open[start_i] = TRUE;

    while (TRUE) {
        i32 current = -1; f32 best_f = INF;
        for (i32 i = 0; i < NT; ++i)
            if (in_open[i] && fscore[i] < best_f) { best_f = fscore[i]; current = i; }
        if (current < 0) return FALSE;     // open set empty -> no route (e.g. undocked cross-hull = SPLIT)
        if (current == goal_i) break;

        in_open[current] = FALSE; in_closed[current] = TRUE;

        // Gather neighbors. Hull nodes: the 4 in-hull orthogonal steps, plus a seam edge to the
        // CONNECTOR if this is a mated door. The connector node itself: edges to both mated doors.
        // Routing door -> connector -> door (never door -> door) threads the path through the bridge.
        i32 nbrs[5]; i32 nn = 0;
        if (current < N) {
            i32 sh, cc, cr; const Ship* sp;
            split(current, &sh, &cc, &cr, &sp);
            for (i32 k = 0; k < 4; ++k) {
                i32 ni = node_of(sh, cc + NAV_DC[k], cr + NAV_DR[k]);
                if (ni >= 0) nbrs[nn++] = ni;
            }
            if ((current == da || current == db) && conn >= 0) nbrs[nn++] = conn; // door -> connector
        } else {
            // The synthetic connector bridge node links to both mated doors.
            if (da >= 0) nbrs[nn++] = da;
            if (db >= 0) nbrs[nn++] = db;
        }

        for (i32 t = 0; t < nn; ++t) {
            const i32 ni = nbrs[t];
            if (!walk[ni] || in_closed[ni]) continue;
            // Edge cost = real world distance between the two tile centers, in tile units. In-hull
            // steps are ~1; the seam edge is the actual mated-door gap (~1 tile). Uniform + exact.
            const f32 step = vec2_length(vec2_sub(wc[ni], wc[current])) / ts;
            const f32 tentative = gscore[current] + step;
            if (tentative < gscore[ni]) {
                came[ni]    = current;
                gscore[ni]  = tentative;
                fscore[ni]  = tentative + heur(ni);
                in_open[ni] = TRUE;
            }
        }
    }

    // Reconstruct goal -> start, emit reversed (start -> goal) as WORLD-space tile centers.
    static i32 chain[NAV_MERGE_MAX];
    i32 count = 0;
    for (i32 t = goal_i; t != -1; t = came[t]) {
        if (count >= NAV_MERGE_MAX) return FALSE; // paranoia: malformed predecessor cycle
        chain[count++] = t;
    }
    if (count > NAV_MAX_PATH) return FALSE; // path longer than the crew's buffer

    for (i32 i = 0; i < count; ++i)
        out_path_world[i] = wc[chain[count - 1 - i]];
    *out_len = count;
    return TRUE;
}
