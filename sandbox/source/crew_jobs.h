#pragma once

#include <defines.h>
#include "job.h"
#include "ship.h"   // TileType — the tile<->job mapping below is declared in terms of it

// =====================================================================================
// Crew Job System — queue operations + the per-frame job runner (Phase 3).
//
// Free functions in the codebase's C-with-structs idiom (cf. ship_*, nav_*). The crew owns
// its job queue and `current` active job (fields added to Crew in game.h); these operate on
// them. The runner is the autonomous "do the assigned work" loop — it pops the next job,
// pathfinds the crew to the job's station, holds it there executing, and maintains the
// crew's is_active_pilot flag that gates global-mode flight (the gate lives in game_update).
// =====================================================================================

struct Crew;        // game.h — extended with skills/queue/current/is_active_pilot
struct game_state;  // game.h — the runner reads the ship (for the station tile + A*)

// ---- Tile <-> Job mapping (the generic, extensible assignment seam) --------------------
// The single source of truth tying a station TILE to the JOB performed there. EVERYTHING in the
// assignment flow — Shift+Right-Click, the runner's target resolution, and the HUD labels — goes
// through these, so adding a job a tile offers is ONE case per tiny switch (no other edits).

// The job a crew member performs at tile `t`, or JOB_NONE if the tile offers no job. Shift+Right-
// Click consults this: hover a TILE_HELM -> JOB_PILOTING.
JobType job_for_tile(TileType t);

// Inverse: the station tile type a job is performed at (TILE_EMPTY if the job needs no station).
// The runner uses this to locate a station when a job was enqueued without an explicit target.
TileType job_station_tile(JobType j);

// Build a job targeted at a specific clicked station tile (col,row in the ship-local grid). The
// result carries has_target=TRUE so the runner walks to THAT exact tile. If the tile offers no
// job the returned job has type JOB_NONE (caller should not enqueue it).
Job job_make_for_tile(TileType t, i32 col, i32 row);

// ---- Display helpers (Crew Job Panel HUD) ---------------------------------------------
// Short human label for a job type ("Piloting", "Idle"). Never NULL.
const char* job_type_name(JobType j);

// Short label for a job state in the player's vocabulary: Queued / Moving / Performing /
// Completed / Failed / Interrupted. Never NULL.
const char* job_state_label(JobState st);

// Name of the station a job is performed at ("Helm"). Never NULL ("-" when station-less).
const char* job_station_name(JobType j);

// 0..1 progression of the crew's current job for the HUD bar: 0 while queued, the path-walk
// fraction while moving, full while performing/completed. 0 when the crew is idle.
f32 crew_job_progress(const Crew* c);

// ---- Queue operations -----------------------------------------------------------------
// Append `job` to the crew's queue. Returns FALSE if the queue is full (CREW_MAX_JOBS).
b8 crew_enqueue_job(Crew* c, Job job);

// Remove the job at slot `idx`, compacting the queue. Returns FALSE if `idx` is out of range.
b8 crew_remove_job(Crew* c, i32 idx);

// Move the job at `idx` one slot toward the front (dir < 0) or back (dir > 0) by swapping
// with its neighbor. Returns FALSE if the move would fall off either end. (FIFO order is the
// dispatch order absent priorities, so reordering directly changes which job runs next.)
b8 crew_reorder_job(Crew* c, i32 idx, i32 dir);

// Drop every queued job. Does NOT touch the crew's `current` (in-flight) job — interrupt that
// separately if needed.
void crew_clear_jobs(Crew* c);

// ---- The runner -----------------------------------------------------------------------
// Advance the crew's job state machine by one frame. Runs in BOTH modes (like simulate_crew),
// so a crew keeps walking to / manning its station while you're zoomed out piloting. Idle &
// queue non-empty -> pop best (priority then FIFO) -> A* to the station -> execute (persist).
// Maintains c->is_active_pilot (TRUE only while a PILOTING job is EXECUTING on the helm tile).
// `dt` is accepted for future time-based job logic; unused this phase.
void crew_update_jobs(game_state* s, Crew* c, f32 dt);
