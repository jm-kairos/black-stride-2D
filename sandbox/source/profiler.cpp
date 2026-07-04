#include "profiler.h"
#include <renderer/bs_ui.h>
#include <renderer/renderer.h>
#include <chrono>
#include <stdio.h>

// Shared group-label pointers so build_ui() can detect group changes by pointer identity
// (string-literal pooling is not guaranteed across separate occurrences).
static const char* G_FRAME  = "FRAME";
static const char* G_UPDATE = "UPDATE";
static const char* G_RENDER = "RENDER";

static inline f64 profiler_now_ns()
{
    return (f64)std::chrono::steady_clock::now().time_since_epoch().count();
}

void Profiler::init()
{
    for (i32 i = 0; i < PROF_ZONE_COUNT; ++i) {
        zones[i].name    = "";
        zones[i].group   = "";
        zones[i].last_ms = 0.0;
        zones[i].avg_ms  = 0.0;
        zones[i].accum_ms = 0.0;
        zones[i].t0      = 0.0;
    }

    enabled  = TRUE;
    expanded = TRUE;
    present_immediate = FALSE;
    wall_ms  = 0.0;
    present_ms = 0.0;

    zones[PROF_UPDATE_TOTAL].name  = "Update total";     zones[PROF_UPDATE_TOTAL].group  = G_FRAME;
    zones[PROF_RENDER_TOTAL].name  = "Render total";     zones[PROF_RENDER_TOTAL].group  = G_FRAME;

    zones[PROF_FLEET_AUTOPILOT].name = "Fleet autopilot"; zones[PROF_FLEET_AUTOPILOT].group = G_UPDATE;
    zones[PROF_FLEET_SIM].name     = "Fleet sim";        zones[PROF_FLEET_SIM].group     = G_UPDATE;
    zones[PROF_SHIP_COLLISION].name = "Ship collision";  zones[PROF_SHIP_COLLISION].group = G_UPDATE;
    zones[PROF_PROJECTILES].name   = "Projectiles";      zones[PROF_PROJECTILES].group   = G_UPDATE;
    zones[PROF_TRAVEL].name        = "Travel";           zones[PROF_TRAVEL].group        = G_UPDATE;
    zones[PROF_RTS].name           = "RTS controls";     zones[PROF_RTS].group           = G_UPDATE;
    zones[PROF_OUT_SENSOR_FX].name = "Out-sensor FX";    zones[PROF_OUT_SENSOR_FX].group = G_UPDATE;

    zones[PROF_STARS].name         = "Stars/galaxy";     zones[PROF_STARS].group         = G_RENDER;
    zones[PROF_BACKGROUND].name    = "Background";       zones[PROF_BACKGROUND].group    = G_RENDER;
    zones[PROF_SHIPS].name         = "Ships";            zones[PROF_SHIPS].group         = G_RENDER;
    zones[PROF_PROJECTILES_DRAW].name = "Projectiles draw"; zones[PROF_PROJECTILES_DRAW].group = G_RENDER;
    zones[PROF_HEAT_MAP].name      = "Heat map";         zones[PROF_HEAT_MAP].group      = G_RENDER;
    zones[PROF_UI].name            = "UI panels";        zones[PROF_UI].group            = G_RENDER;
}

void Profiler::begin_frame()
{
    if (!enabled) return;
    for (i32 i = 0; i < PROF_ZONE_COUNT; ++i) {
        ProfileZone& z = zones[i];
        z.last_ms  = z.accum_ms;
        z.avg_ms   = z.avg_ms * 0.9 + z.accum_ms * 0.1;
        z.accum_ms = 0.0;
    }
}

void Profiler::set_wall_dt(f32 dt)
{
    f64 dt_ms = (f64)dt * 1000.0;
    // Ignore absurd deltas (first frame, breakpoints) so the average stays meaningful.
    if (dt_ms <= 0.0 || dt_ms > 1000.0) return;
    wall_ms = (wall_ms <= 0.0) ? dt_ms : (wall_ms * 0.9 + dt_ms * 0.1);
}

void Profiler::set_present_ms(f32 ms)
{
    f64 v = (f64)ms;
    if (v < 0.0 || v > 1000.0) return;
    present_ms = (present_ms <= 0.0) ? v : (present_ms * 0.9 + v * 0.1);
}

void Profiler::begin(ProfileZoneId id)
{
    if (!enabled) return;
    zones[id].t0 = profiler_now_ns();
}

void Profiler::end(ProfileZoneId id)
{
    if (!enabled) return;
    zones[id].accum_ms += (profiler_now_ns() - zones[id].t0) / 1.0e6;
}

void Profiler::build_ui()
{
    char buf[112];

    bs_ui_text_colored(0.55f, 0.95f, 0.75f, 1.0f, "PROFILER");

    // Real frame rate: wall-clock delta including the VSYNC / frame-cap wait.
    snprintf(buf, sizeof(buf), "Frame: %.2f ms  (%.0f fps)",
             wall_ms, wall_ms > 0.0 ? 1000.0 / wall_ms : 0.0);
    bs_ui_text_colored(0.45f, 0.85f, 0.95f, 1.0f, buf);

    // CPU work per frame (update + render), independent of the frame-rate cap.
    f64 frame_total = zones[PROF_UPDATE_TOTAL].last_ms + zones[PROF_RENDER_TOTAL].last_ms;
    snprintf(buf, sizeof(buf), "CPU:   %.2f ms", frame_total);
    bs_ui_text_colored(0.60f, 0.70f, 0.80f, 1.0f, buf);

    // Present: engine end_frame time (submit + present + VSYNC wait). When VSYNC is on this
    // includes the vblank block; in IMMEDIATE mode it approximates GPU submit+execute time.
    snprintf(buf, sizeof(buf), "Prsnt: %.2f ms", present_ms);
    bs_ui_text_colored(0.90f, 0.75f, 0.45f, 1.0f, buf);

    // Toggle VSYNC <-> IMMEDIATE. IMMEDIATE uncaps the loop so Prsnt reflects true GPU time.
    if (bs_ui_button(present_immediate ? "GPU mode: IMMEDIATE" : "GPU mode: VSYNC", TRUE)) {
        present_immediate = !present_immediate;
        renderer_set_present_mode(present_immediate);
    }

    if (bs_ui_button(expanded ? "Collapse" : "Expand", TRUE)) expanded = !expanded;
    if (!expanded) return;

    const char* cur_group = 0;
    for (i32 i = 0; i < PROF_ZONE_COUNT; ++i) {
        ProfileZone& z = zones[i];
        if (z.last_ms <= 0.0 && z.avg_ms <= 0.0) continue; // zone not exercised

        if (z.group != cur_group) {
            bs_ui_text_colored(0.55f, 0.62f, 0.72f, 0.9f, z.group);
            cur_group = z.group;
        }

        snprintf(buf, sizeof(buf), "  %-16s %6.2f ms  (avg %5.2f)", z.name, z.last_ms, z.avg_ms);
        bs_ui_text(buf);
    }
}
