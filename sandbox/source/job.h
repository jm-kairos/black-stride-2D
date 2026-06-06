#pragma once

#include <defines.h>

// =====================================================================================
// Crew Job System — data model (Phase 3).
//
// A "job" is a unit of autonomous work a crew member performs without direct control:
// the crew pathfinds to a station and then executes the task there. This mirrors the
// select-and-order / colony-sim model (the prototype's north star) — you ASSIGN work,
// and the crew carries it out across the decks of the moving, rotating hull while you do
// other things (e.g. zoom out and fly).
//
// The user's class/std::queue/enum-class examples are rendered here in the codebase's
// C-with-structs idiom: PODs, enum tags, fixed-size arrays, free functions (consistent
// with ship/nav/crew). Queue ops + the per-frame runner live in crew_jobs.{h,cpp}; the
// Crew owns its queue + current job (see game.h).
//
// Phase 3 ships exactly one job type — JOB_PILOTING — which is PERSISTENT: it reaches
// JOB_EXECUTING (crew stood at the helm) and stays there until interrupted/unassigned (it
// does not auto-COMPLETE). The other type/state/skill values are named seams the runner
// and panel already accommodate, not yet built.
// =====================================================================================

enum JobType {
    JOB_NONE = 0,      // empty slot / no job
    JOB_PILOTING       // walk to the helm and man it (enables global-mode flight)
    // , JOB_REPAIR, JOB_MAN_STATION, ...  (deferred seams)
};

enum JobState {
    JOB_QUEUED = 0,        // freshly popped into `current`; not yet resolved to a target
    JOB_MOVING_TO_TARGET,  // A* path issued; crew is walking to the station tile
    JOB_EXECUTING,         // crew arrived; performing the task (PILOTING persists here)
    JOB_COMPLETED,         // one-shot job finished (unused by PILOTING; seam for later types)
    JOB_FAILED,            // no station / no path — could not be carried out
    JOB_INTERRUPTED        // pre-empted (e.g. a manual move order walked the pilot away)
};

// A single queued/active job. POD — copied by value into/out of the crew's queue.
struct Job {
    JobType  type;
    JobState state;
    b8       has_target;            // does this job resolve to a tile target?
    i32      target_col, target_row; // station tile (ship-local grid); valid when has_target
    i32      priority;             // 0 = normal; higher pulls ahead of FIFO dispatch
};

// Skills are a stub this phase: a level per skill, no gameplay effect yet (progression is a
// named seam). One entry today (piloting).
enum SkillType {
    SKILL_PILOTING = 0,
    SKILL_COUNT
};

struct SkillSet {
    u8 level[SKILL_COUNT];
};

// Crew Job Panel action intents. The panel (build_crew_job_panel in game.cpp) returns one of
// these from a clicked button; apply_crew_job_action maps it onto the crew_jobs.* queue ops.
// `param` disambiguates (the job slot index for the per-row reorder/remove buttons; 0 for the
// global assign/cancel actions). Relocated here from the retired ui.h when the bespoke widget
// layer was replaced by the ImGui-backed bs_ui_* facade (Task 9).
typedef enum UiAction {
    UI_ACTION_NONE = 0,
    UI_ACTION_ASSIGN_PILOTING,  // enqueue a PilotingJob on the selected crew
    UI_ACTION_REMOVE_JOB,       // remove job slot `param` from the selected crew
    UI_ACTION_REORDER_UP,       // move job slot `param` earlier
    UI_ACTION_REORDER_DOWN,     // move job slot `param` later
    UI_ACTION_CANCEL_CURRENT,   // interrupt the crew's in-flight `current` job
} UiAction;
