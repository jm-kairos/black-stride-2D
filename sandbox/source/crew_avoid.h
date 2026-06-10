#pragma once

#include <defines.h>
#include <math/math_utils.h>

// =====================================================================================
// Crew avoidance — multi-agent collision avoidance + deadlock resolution (scalable to many
// crew, not just two). Layered on top of the existing single-agent A* (nav_*) and continuous
// steering (simulate_crew). THREE cooperating tiers, cheapest-first:
//
//   1. PLANNING-TIME avoidance (crew_plan_path / crew_replan_around): when a crew member plans
//      a route, the tiles other crew OCCUPY or have RESERVED (their goal) are fed to
//      nav_find_path_avoiding as impassable, so two crew never PLAN onto the same tile or a
//      head-through route when an alternative exists. Runs only when an order is issued (every
//      few seconds), so it's effectively free.
//
//   2. PER-FRAME GATE (crew_peer_blocks): a physical guarantee. simulate_crew calls this per
//      movement axis (exactly mirroring the crew-vs-solid-tile gate) and cancels any step that
//      would overlap another crew member. This makes overlap / cross-through IMPOSSIBLE
//      regardless of what the paths say, for ANY number of agents — and because it's per-axis
//      and only vetoes motion that REDUCES separation, agents slide around each other in open
//      space instead of sticking. O(crew^2) of cheap vector math per frame (256 ops at 16 crew).
//
//   3. DEADLOCK RESOLUTION (crew_resolve_deadlocks): the gate can leave two agents stuck facing
//      off in a tight spot. Each crew member tracks how long it has been gate-blocked while
//      trying to move; past a threshold it (a) tries to REPLAN AROUND the blockers, and if no
//      detour exists (b) the LOWER-PRIORITY agent YIELDS (briefly waits in place) so the higher-
//      priority one passes — a stable priority (crew index) breaks the symmetry so they can't
//      livelock. An agent that stays hopelessly stuck eventually ABORTS its order (a job then
//      fails cleanly) rather than freezing. A* here fires only on the block threshold, so the
//      per-frame cost is ~zero in the common case.
//
// All state lives on Crew (game.h): dest_col/dest_row/has_dest (replan goal), block_timer,
// wait_timer, stuck_timer. The system is invoked from game_update; see game.cpp wiring.
// =====================================================================================

struct game_state;  // game.h
struct Crew;        // game.h

// ---- Tier 1: planning ------------------------------------------------------------------
// Plan an avoidance-aware path for crew member `self_index` to the goal TILE (goal_col,goal_row).
// Routes around other crew's occupied + reserved tiles. On success installs c->path/path_len/
// path_idx, records (goal_col,goal_row) as the crew's destination for later replanning, resets
// the avoidance timers, and returns TRUE. If no peer-avoiding route exists (a peer momentarily
// sits on the only corridor) it FALLS BACK to a plain static path so the order is still honored —
// the Tier-2 gate then prevents overlap and Tier-3 manages the squeeze. Returns FALSE only when
// even the static path fails (endpoint not walkable / genuinely unreachable), leaving the crew's
// existing path untouched (same contract as nav_find_path). Use for NEW orders (move command,
// job dispatch).
b8 crew_plan_path(game_state* s, i32 self_index, i32 goal_col, i32 goal_row);

// ---- Tier 2: per-frame physical gate ---------------------------------------------------
// TRUE if moving crew `self_index` from ship-local center `from` to candidate center `cand`
// (radius `r`) would collide with another crew member: it would come within (r + peer.radius)
// of a peer AND end up closer to that peer than `from` already is. The "closer than before"
// clause means an already-touching/overlapping pair is never frozen — they can always move
// APART, only not together. simulate_crew calls this once per axis so blocked agents slide.
b8 crew_peer_blocks(const game_state* s, i32 self_index, bs_math::Vec2 from, bs_math::Vec2 cand, f32 r);

// ---- Tier 3: deadlock resolution -------------------------------------------------------
// Once-per-frame pass over all crew. For any crew member that has been gate-blocked by a peer
// for longer than the deadlock threshold, attempt a detour (replan around the blockers); failing
// that, make the lower-priority agent yield (briefly wait) so the higher-priority one passes, and
// abort an order that stays hopelessly stuck. Maintains wait_timer/stuck_timer. Call from
// game_update AFTER simulate_crew (which maintains each crew's block_timer). dt = frame time.
void crew_resolve_deadlocks(game_state* s, f32 dt);
