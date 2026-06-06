#include "crew_jobs.h"
#include "game.h"
#include "nav.h"

#include <core/logger.h>

using namespace bs_math;

// =====================================================================================
// Tile <-> Job mapping + display helpers (the generic assignment seam). Adding a job a tile
// offers = one case per switch below; the assignment path, runner, and HUD need no other edit.
// =====================================================================================
JobType job_for_tile(TileType t) {
    switch (t) {
        case TILE_HELM: return JOB_PILOTING;     // man the helm -> Piloting
        // case TILE_REPAIR_BAY: return JOB_REPAIR;  (future stations slot in here)
        default:        return JOB_NONE;          // tile offers no job
    }
}

TileType job_station_tile(JobType j) {
    switch (j) {
        case JOB_PILOTING: return TILE_HELM;
        default:           return TILE_EMPTY;     // station-less / unknown
    }
}

Job job_make_for_tile(TileType t, i32 col, i32 row) {
    Job job{};
    job.type     = job_for_tile(t);
    job.state    = JOB_QUEUED;
    job.priority = 0;
    if (job.type != JOB_NONE) {                   // remember the EXACT clicked tile as the target
        job.has_target = TRUE;
        job.target_col = col;
        job.target_row = row;
    }
    return job;
}

const char* job_type_name(JobType j) {
    switch (j) {
        case JOB_PILOTING: return "Piloting";
        case JOB_NONE:     return "Idle";
        default:           return "?";
    }
}

const char* job_state_label(JobState st) {
    switch (st) {
        case JOB_QUEUED:           return "Queued";
        case JOB_MOVING_TO_TARGET: return "Moving";
        case JOB_EXECUTING:        return "Performing";
        case JOB_COMPLETED:        return "Completed";
        case JOB_FAILED:           return "Failed";
        case JOB_INTERRUPTED:      return "Interrupted";
        default:                   return "?";
    }
}

const char* job_station_name(JobType j) {
    switch (j) {
        case JOB_PILOTING: return "Helm";
        default:           return "-";
    }
}

f32 crew_job_progress(const Crew* c) {
    if (!c || !c->has_current) return 0.0f;
    switch (c->current.state) {
        case JOB_QUEUED:           return 0.0f;
        case JOB_MOVING_TO_TARGET: {
            // Fraction of the A* route already walked (path_idx of path_len-1 segments). A short
            // or just-cleared path reads as "basically arrived" so the bar never stalls near full.
            if (c->path_len <= 1) return 1.0f;
            f32 frac = (f32)c->path_idx / (f32)(c->path_len - 1);
            return (frac < 0.0f) ? 0.0f : (frac > 1.0f) ? 1.0f : frac;
        }
        case JOB_EXECUTING:        return 1.0f;  // on station, performing -> full bar
        case JOB_COMPLETED:        return 1.0f;
        default:                   return 0.0f;  // failed / interrupted
    }
}

// =====================================================================================
// Queue operations (fixed-size array; FIFO unless a job's priority pulls it ahead).
// =====================================================================================
b8 crew_enqueue_job(Crew* c, Job job) {
    if (!c) return FALSE;
    if (c->job_count >= CREW_MAX_JOBS) return FALSE;
    job.state = JOB_QUEUED;            // a freshly queued job always starts QUEUED
    c->queue[c->job_count++] = job;
    return TRUE;
}

b8 crew_remove_job(Crew* c, i32 idx) {
    if (!c || idx < 0 || idx >= c->job_count) return FALSE;
    for (i32 i = idx; i < c->job_count - 1; ++i) // compact the tail down one slot
        c->queue[i] = c->queue[i + 1];
    c->job_count--;
    return TRUE;
}

b8 crew_reorder_job(Crew* c, i32 idx, i32 dir) {
    if (!c || idx < 0 || idx >= c->job_count) return FALSE;
    i32 j = (dir < 0) ? idx - 1 : idx + 1; // neighbor toward front (dir<0) or back (dir>0)
    if (j < 0 || j >= c->job_count) return FALSE;
    Job tmp      = c->queue[idx];
    c->queue[idx] = c->queue[j];
    c->queue[j]   = tmp;
    return TRUE;
}

void crew_clear_jobs(Crew* c) {
    if (!c) return;
    c->job_count = 0; // leaves `current` (the in-flight job) alone by design
}

// Pick the index of the next job to dispatch: highest priority, FIFO tiebreak (the strict `>`
// keeps the earliest-enqueued among equal priorities). Returns -1 when the queue is empty.
static i32 pick_best_job(const Crew* c) {
    i32 best = -1;
    for (i32 i = 0; i < c->job_count; ++i)
        if (best < 0 || c->queue[i].priority > c->queue[best].priority)
            best = i;
    return best;
}

// =====================================================================================
// The runner — one state transition per frame. Runs in BOTH modes (cf. simulate_crew), so a
// crew keeps walking to / manning its station while you're zoomed out piloting. It ISSUES the
// crew's A* path (which simulate_crew then steers along) and maintains is_active_pilot.
// =====================================================================================
void crew_update_jobs(game_state* s, Crew* c, f32 dt) {
    (void)dt; // no time-based job logic this phase (seam for durations/cooldowns later)
    if (!s || !c) return;
    Ship* ship = &s->ship;

    // ---- Idle & queue non-empty: pop the best job into `current` ----
    if (!c->has_current) {
        c->is_active_pilot = FALSE;
        i32 idx = pick_best_job(c);
        if (idx < 0) return;                 // nothing queued -> stay idle
        c->current       = c->queue[idx];
        c->current.state = JOB_QUEUED;
        c->has_current   = TRUE;
        crew_remove_job(c, idx);
        // fall through and resolve the QUEUED job the same frame (responsive dispatch)
    }

    switch (c->current.state) {
        // ---- QUEUED: resolve the station tile, run A* from the crew's tile to it ----
        case JOB_QUEUED: {
            c->is_active_pilot = FALSE;

            // Resolve the target station tile. A job assigned by Shift+Right-Click already carries
            // the EXACT clicked tile (has_target); honor it so the crew goes where the player
            // pointed. A target-less job (e.g. a panel "Assign" button) falls back to the first
            // station of the job's type on the ship. Either way we end with (tcol,trow) + a
            // validity check that the tile really offers this job (guards a stale/empty target).
            i32 tcol = -1, trow = -1;
            b8  have_target = FALSE;
            if (c->current.has_target) {
                tcol = c->current.target_col;
                trow = c->current.target_row;
                have_target = (job_for_tile(ship_tile_at(ship, tcol, trow)) == c->current.type);
            } else {
                TileType station = job_station_tile(c->current.type);
                if (station != TILE_EMPTY)
                    have_target = ship_find_first_tile(ship, station, &tcol, &trow);
            }

            if (!have_target) {              // no such station on the ship / stale target
                c->current.state = JOB_FAILED;
                break;
            }
            c->current.has_target = TRUE;
            c->current.target_col = tcol;
            c->current.target_row = trow;

            i32 scol, srow;
            ship_local_to_tile(ship, c->position, &scol, &srow);

            i32 len = 0;
            if (nav_find_path(ship, scol, srow, tcol, trow, c->path, &len)) {
                c->path_len      = len;
                c->path_idx      = 0;        // path[0] is the crew's own tile; arrival advances at once
                c->current.state = JOB_MOVING_TO_TARGET;
            } else {                          // station unreachable (walled off)
                c->current.state = JOB_FAILED;
            }
        } break;

        // ---- MOVING_TO_TARGET: wait for simulate_crew to consume the path (path_len -> 0) ----
        case JOB_MOVING_TO_TARGET: {
            c->is_active_pilot = FALSE;
            if (c->path_len == 0) {           // simulate_crew cleared it on reaching the last waypoint
                i32 ccol, crow;
                ship_local_to_tile(ship, c->position, &ccol, &crow);
                if (ccol == c->current.target_col && crow == c->current.target_row)
                    c->current.state = JOB_EXECUTING;
                else
                    c->current.state = JOB_FAILED; // path ended off-target (defensive; shouldn't happen)
            }
        } break;

        // ---- EXECUTING: PILOTING persists here and holds the pilot flag. is_active_pilot is the
        // Phase 4 flight gate: control_ship_global (game.cpp) only runs while it is TRUE, so global-
        // mode flight is dead unless a crew member is actively manning the helm right here. ----
        case JOB_EXECUTING: {
            // A manual move order (right-click) repopulates the crew's path while parked at the
            // station — treat it as walking the pilot away: interrupt the job and drop the flag.
            if (c->path_len > 0) {
                c->current.state   = JOB_INTERRUPTED;
                c->is_active_pilot = FALSE;
                break;
            }
            c->is_active_pilot = (c->current.type == JOB_PILOTING) ? TRUE : FALSE;
        } break;

        // ---- Terminal states: clear `current`; next frame the runner pops the next queued job ----
        case JOB_COMPLETED:
        case JOB_FAILED:
        case JOB_INTERRUPTED:
        default: {
            c->is_active_pilot = FALSE;
            c->has_current     = FALSE;
            c->current         = Job{};
            c->current.type    = JOB_NONE;
        } break;
    }
}
