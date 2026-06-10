#include "crew_avoid.h"
#include "game.h"
#include "nav.h"

#include <core/logger.h>

using namespace bs_math;

// =====================================================================================
// Tuning. Units are ship-local (tile_size 32; crew radius 10; crew max speed ~100/s).
// =====================================================================================
static const f32 AVOID_BLOCK_THRESHOLD = 0.35f; // gate-blocked this long (s) -> act (replan/yield)
static const f32 AVOID_WAIT_DURATION   = 0.60f; // a yielding agent pauses this long (s) to let a peer pass
static const f32 AVOID_STUCK_ABORT     = 6.00f; // hopelessly stuck this long (s) -> abort the order
static const f32 AVOID_PRIORITY_SLACK  = 6.00f; // extra reach (units) for "who is blocking me" scan

// Max dynamic-obstacle tiles handed to the avoidance A*: each peer contributes up to 2 (the tile
// it stands on + the tile it has reserved as its destination). Comfortably covers 20+ crew.
#define AVOID_MAX_BLOCKED 64

// ---- small helpers ---------------------------------------------------------------------
static b8 crew_actively_moving(const Crew* c) {
    return (c->path_len > 0 && c->path_idx < c->path_len) ? TRUE : FALSE;
}

// Dispatch priority of a crew member: its active job's priority, else 0 (idle/manual move).
static i32 crew_priority(const Crew* c) {
    return c->has_current ? c->current.priority : 0;
}

// Does peer `j` out-rank self `i` for right-of-way? Higher job priority wins; ties break by the
// LOWER crew index (stable, so two equal-priority agents can't both decide to yield -> no livelock).
static b8 peer_outranks(const game_state* s, i32 i, i32 j) {
    const i32 pi = crew_priority(&s->crew[i]);
    const i32 pj = crew_priority(&s->crew[j]);
    if (pj != pi) return (pj > pi) ? TRUE : FALSE;
    return (j < i) ? TRUE : FALSE;
}

// Collect the tiles OTHER crew occupy (their current tile) or have reserved (their destination
// tile), as impassable input for the avoidance A*. `self_index` is excluded. Returns the count.
// ONLY peers on the SAME hull as `self` count: a crew on the enemy deck lives in a different local
// frame, so its tile indices are meaningless on this hull's grid (and it's physically a hull away).
static i32 gather_peer_obstacles(const game_state* s, i32 self_index,
                                 i32* out_cols, i32* out_rows) {
    const Crew* self = &s->crew[self_index];
    const Ship* ship = crew_ship(s, self);
    i32 n = 0;
    const i32 crew_n = (i32)s->crew.size();
    for (i32 j = 0; j < crew_n; ++j) {
        if (j == self_index) continue;
        const Crew* p = &s->crew[j];
        if (p->ship_id != self->ship_id) continue; // different hull -> not an obstacle on this grid

        // Tile the peer currently stands on — never plan a route through a crewmate.
        i32 pc, pr;
        ship_local_to_tile(ship, p->position, &pc, &pr);
        if (n < AVOID_MAX_BLOCKED) { out_cols[n] = pc; out_rows[n] = pr; ++n; }

        // Tile the peer has reserved as its goal — never plan to END on a peer's destination.
        if (p->has_dest && n < AVOID_MAX_BLOCKED) {
            out_cols[n] = p->dest_col; out_rows[n] = p->dest_row; ++n;
        }
    }
    return n;
}

// Run the AVOIDANCE A* (peers' tiles impassable) for crew `i` to (goal_col,goal_row). On success
// installs the path and returns TRUE. Returns FALSE if no peer-avoiding route exists (caller
// decides whether to fall back to a static path or to yield). Does NOT touch the avoidance timers.
static b8 plan_avoiding(game_state* s, i32 i, i32 goal_col, i32 goal_row) {
    Crew* c = &s->crew[i];
    const Ship* ship = crew_ship(s, c); // plan on the hull THIS crew stands on

    i32 start_col, start_row;
    ship_local_to_tile(ship, c->position, &start_col, &start_row);

    static i32 bcols[AVOID_MAX_BLOCKED];
    static i32 brows[AVOID_MAX_BLOCKED];
    const i32 bcount = gather_peer_obstacles(s, i, bcols, brows);

    i32 len = 0;
    if (nav_find_path_avoiding(ship, start_col, start_row, goal_col, goal_row,
                               bcols, brows, bcount, c->path, &len)) {
        c->path_len = len;
        c->path_idx = 0;
        return TRUE;
    }
    return FALSE;
}

// =====================================================================================
// Tier 1: plan an avoidance-aware path for a NEW order. Avoidance first; static fallback so the
// order is always honored (the per-frame gate then guarantees no overlap, Tier 3 unsticks it).
// =====================================================================================
b8 crew_plan_path(game_state* s, i32 self_index, i32 goal_col, i32 goal_row) {
    if (!s) return FALSE;
    if (self_index < 0 || self_index >= (i32)s->crew.size()) return FALSE;
    Crew* c = &s->crew[self_index];

    b8 ok = plan_avoiding(s, self_index, goal_col, goal_row);
    if (!ok) {
        // No peer-avoiding route (a crewmate momentarily blocks the only corridor). Honor the
        // order with a plain static path; the gate prevents overlap and Tier 3 resolves the squeeze.
        const Ship* ship = crew_ship(s, c); // the hull this crew stands on
        i32 start_col, start_row;
        ship_local_to_tile(ship, c->position, &start_col, &start_row);
        i32 len = 0;
        if (nav_find_path(ship, start_col, start_row, goal_col, goal_row, c->path, &len)) {
            c->path_len = len;
            c->path_idx = 0;
            ok = TRUE;
        }
    }
    if (ok) {
        // Record the destination so Tier 3 can replan toward it, and reset the avoidance clocks.
        c->dest_col     = goal_col;
        c->dest_row     = goal_row;
        c->has_dest     = TRUE;
        c->block_timer  = 0.0f;
        c->wait_timer   = 0.0f;
        c->stuck_timer  = 0.0f;
    }
    return ok;
}

// =====================================================================================
// Tier 2: per-frame physical gate. Veto a step that would push crew `self_index` INTO a peer
// (within r+peer.r) AND closer than it already is — so an overlapping pair can always separate
// but never converge. Called once per movement axis by simulate_crew.
// =====================================================================================
b8 crew_peer_blocks(const game_state* s, i32 self_index, Vec2 from, Vec2 cand, f32 r) {
    const Crew* self = &s->crew[self_index];
    const i32 crew_n = (i32)s->crew.size();
    for (i32 j = 0; j < crew_n; ++j) {
        if (j == self_index) continue;
        const Crew* p = &s->crew[j];
        if (p->ship_id != self->ship_id) continue;  // different hull: positions live in different frames
        const f32 rr = r + p->radius;

        const Vec2 dc = vec2_sub(cand, p->position);
        const f32  dist_c = vec2_length(dc);
        if (dist_c >= rr) continue;                 // candidate doesn't overlap this peer

        const Vec2 df = vec2_sub(from, p->position);
        const f32  dist_f = vec2_length(df);
        if (dist_c < dist_f) return TRUE;           // moving CLOSER into an overlap -> veto
        // else: already overlapping but moving apart (or tangential) -> allow, so they unstick
    }
    return FALSE;
}

// =====================================================================================
// Tier 3: deadlock resolution. Once per frame, after simulate_crew (which maintains block_timer).
// =====================================================================================

// Is any peer close enough to be the thing blocking crew `i` right now (for the yield decision)?
// Writes TRUE to *out_outranked if at least one such blocker out-ranks `i`.
static b8 has_blocking_peer(const game_state* s, i32 i, b8* out_outranked) {
    const Crew* c = &s->crew[i];
    const i32 crew_n = (i32)s->crew.size();
    b8 any = FALSE, outranked = FALSE;
    for (i32 j = 0; j < crew_n; ++j) {
        if (j == i) continue;
        const Crew* p = &s->crew[j];
        if (p->ship_id != c->ship_id) continue;     // different hull: not a physical blocker here
        const f32 reach = c->radius + p->radius + AVOID_PRIORITY_SLACK;
        if (vec2_length(vec2_sub(p->position, c->position)) <= reach) {
            any = TRUE;
            if (peer_outranks(s, i, j)) outranked = TRUE;
        }
    }
    if (out_outranked) *out_outranked = outranked;
    return any;
}

// Abort crew `i`'s current order: stop moving and drop the destination. A crew member with an
// active JOB sees path_len==0 next frame and the job fails cleanly (runner: off-target -> FAILED).
static void abort_order(Crew* c) {
    c->path_len    = 0;
    c->path_idx    = 0;
    c->has_dest    = FALSE;
    c->velocity    = Vec2{ 0.0f, 0.0f };
    c->block_timer = 0.0f;
    c->wait_timer  = 0.0f;
    c->stuck_timer = 0.0f;
}

void crew_resolve_deadlocks(game_state* s, f32 dt) {
    if (!s) return;
    const i32 crew_n = (i32)s->crew.size();
    for (i32 i = 0; i < crew_n; ++i) {
        Crew* c = &s->crew[i];

        // Not trying to go anywhere -> nothing to deadlock on; keep the clocks clear.
        if (!crew_actively_moving(c)) {
            c->block_timer = 0.0f;
            c->wait_timer  = 0.0f;
            c->stuck_timer = 0.0f;
            continue;
        }

        // Currently yielding: count down the pause and let simulate_crew hold it in place. The
        // wait gives a higher-priority peer time to move through the contested space.
        if (c->wait_timer > 0.0f) {
            c->wait_timer -= dt;
            if (c->wait_timer < 0.0f) c->wait_timer = 0.0f;
            c->stuck_timer += dt;             // still not progressing -> accrue toward abort
            if (c->stuck_timer > AVOID_STUCK_ABORT) {
                BS_LOG_WARN("avoid: crew %d gave up a blocked order (waited too long)", i);
                abort_order(c);
            }
            continue;
        }

        // Not blocked long enough yet -> let it keep steering (the gate handles the brush-past).
        if (c->block_timer < AVOID_BLOCK_THRESHOLD) continue;

        // --- Stuck against a peer. 1) Try to detour around the blockers. ---
        c->stuck_timer += dt;
        if (c->has_dest && plan_avoiding(s, i, c->dest_col, c->dest_row)) {
            c->block_timer = 0.0f;            // fresh route around the obstruction
            // keep stuck_timer: a crew that has to keep re-detouring is still making poor progress,
            // so it can still eventually abort if the detours never actually clear it.
            continue;
        }

        // --- 2) No detour exists. Lower-priority agent yields so the higher-priority one passes. ---
        b8 outranked = FALSE;
        b8 blocked_by_peer = has_blocking_peer(s, i, &outranked);
        if (blocked_by_peer && outranked) {
            c->wait_timer  = AVOID_WAIT_DURATION; // step back / hold so the peer can clear the pinch
            c->block_timer = 0.0f;
            c->velocity    = Vec2{ 0.0f, 0.0f };
        } else {
            // We out-rank everyone in the way (or nothing identifiable is) -> hold position and keep
            // pushing; the gate still prevents overlap and the peers will yield on their own pass.
            c->block_timer = 0.0f;
        }

        // --- 3) Hopelessly stuck (e.g. a true 1-wide head-on) -> abort so we don't freeze forever. ---
        if (c->stuck_timer > AVOID_STUCK_ABORT) {
            BS_LOG_WARN("avoid: crew %d gave up a blocked order (no detour, no yield resolved it)", i);
            abort_order(c);
        }
    }
}
