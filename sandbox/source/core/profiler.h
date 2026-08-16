#pragma once
#include <defines.h>

// =====================================================================================
// Profiler — a lightweight per-frame CPU profiler for the game's major subsystems.
//
// Allocation-free: a fixed table of named zones. Each zone accumulates elapsed time across
// the frame; begin_frame() finalizes the previous frame's accumulators into a rolling
// average and resets them for the new frame, so no explicit end_frame() is needed and there
// is no ordering dependency between the update and render passes.
//
// Timing uses std::chrono::steady_clock (the same source the rest of the sandbox uses for
// frame_ms / heat_map_cpu_ms); platform_get_absolute_time() is engine-internal and not
// exported to the sandbox.
//
// Usage:
//   s->profiler.begin_frame();                 // once, at the top of game_update
//   BS_PROFILE(&s->profiler, PROF_UPDATE_TOTAL); // RAII scope timer (covers all exits)
//   ...
//   s->profiler.begin(PROF_FLEET_SIM);         // explicit pair around a single call
//   s->fleet_state.fleet.simulate_all(...);
//   s->profiler.end(PROF_FLEET_SIM);
// =====================================================================================

enum ProfileZoneId {
    // ---- Frame totals ----
    PROF_UPDATE_TOTAL = 0,
    PROF_RENDER_TOTAL,
    // ---- Render subsystems (listed first: the primary profiling focus) ----
    PROF_STARS,
    PROF_BACKGROUND,
    PROF_SHIPS,
    PROF_PROJECTILES_DRAW,
    PROF_HEAT_MAP,
    PROF_UI,
    // ---- Update subsystems ----
    PROF_FLEET_AUTOPILOT,
    PROF_FLEET_SIM,
    PROF_SHIP_COLLISION,
    PROF_PROJECTILES,
    PROF_TRAVEL,
    PROF_RTS,
    PROF_OUT_SENSOR_FX,
    PROF_ZONE_COUNT
};

struct ProfileZone {
    const char* name;   // display label
    const char* group;  // section header (FRAME / UPDATE / RENDER)
    f64 last_ms;        // total time this zone took in the previous frame
    f64 avg_ms;         // rolling average (0.9 old / 0.1 new)
    f64 accum_ms;       // time accumulated so far in the current frame
    f64 t0;             // steady_clock nanoseconds captured at the last begin()
};

struct Profiler {
    ProfileZone zones[PROF_ZONE_COUNT];
    b8 enabled;
    b8 expanded;        // UI: whether the per-zone breakdown is shown
    b8 present_immediate; // UI mirror: TRUE when swapchain forced to IMMEDIATE (uncapped)
    f64 wall_ms;        // real wall-clock frame time (incl. VSYNC/cap wait), rolling avg
    f64 present_ms;     // engine end_frame time (submit + present + VSYNC wait), rolling avg
    f64 acquire_ms;     // swapchain-image acquire block inside end_frame (VSYNC wait), rolling avg
    f64 submit_ms;      // command-buffer submit inside end_frame, rolling avg

    void init();
    void begin_frame();          // finalize previous frame + reset accumulators
    void set_wall_dt(f32 dt);    // feed the engine's real frame delta (seconds)
    void set_present_ms(f32 ms); // feed the engine's end_frame/present time (ms)
    void set_present_breakdown(f32 acq_ms, f32 sub_ms); // feed the acquire/submit split (ms)
    void begin(ProfileZoneId id);
    void end(ProfileZoneId id);
    void build_ui();             // emit panel body (call between bs_ui_begin/end_panel)
};

// RAII scope timer. Place BS_PROFILE(&s->profiler, PROF_X); at the top of a scope; the zone
// is timed until the scope exits (handles early returns cleanly).
struct ScopedProfile {
    Profiler* p;
    ProfileZoneId id;
    ScopedProfile(Profiler* prof, ProfileZoneId zid) : p(prof), id(zid) { p->begin(id); }
    ~ScopedProfile() { p->end(id); }
};
#define BS_PROF_CAT2(a, b) a##b
#define BS_PROF_CAT(a, b) BS_PROF_CAT2(a, b)
#define BS_PROFILE(prof, id) ScopedProfile BS_PROF_CAT(bs_prof_scope_, __LINE__)((prof), (id))
