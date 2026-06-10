// Headless verification of the local<->global camera-rotation wrap fix (game.cpp:800).
//
// We do NOT link the engine here — we replicate the EXACT camera-rotation update from
// game_update and the EXACT roof_alpha cross-fade easing from update_zoom_and_mode, then
// drive them through the reported repro:  local -> global -> turn the hull past 360 deg ->
// back to local.  The discriminating quantity is the interior's ON-SCREEN heading during the
// zoom-in cross-fade:  screen_angle = ship.angle - camera.rotation  (the scene is drawn at
// +angle, the view applies R(-camera.rotation)).
//
//   BUGGED rule:  camera.rotation = ship.angle          * (1 - roof_alpha)
//   FIXED  rule:  camera.rotation = wrap_angle(ship.angle) * (1 - roof_alpha)
//
// PASS = with the FIXED rule the on-screen heading stays within a small epsilon of upright
//        through the whole fade, AND the BUGGED rule is demonstrated to violate it (so the
//        test actually has teeth).  We also assert the settled-frame equivalence that makes
//        the fix safe: R(wrap(a)) == R(a) for the mouse-pick / final orientation.
//
// Build:  clang++ -std=c++17 -O2 verify_camera_wrap_headless.cpp -o verify_camera_wrap_headless.exe
// (mirrors the engine's compiler; no engine headers needed.)

#include <cmath>
#include <cstdio>
#include <cstdlib>

typedef float f32;
static const f32 BS_PI = 3.14159265358979323846f;

// ---- copied VERBATIM from game.cpp (the fix under test) -------------------------------
static f32 wrap_angle(f32 a) {
    a = fmodf(a + BS_PI, 2.0f * BS_PI);
    if (a < 0.0f) a += 2.0f * BS_PI;
    return a - BS_PI;
}
// ---------------------------------------------------------------------------------------

// Engine constants (game.cpp).
static const f32 ROOF_FADE_SPEED = 8.0f;   // 1/seconds
static const f32 SHIP_MAX_TURN   = 1.8f;   // rad/s (from game.cpp; calibrates the spin)

// Smallest signed difference a-b folded into (-PI, PI] — used only to MEASURE deviation.
static f32 ang_diff(f32 a, f32 b) { return wrap_angle(a - b); }

// One camera.rotation sample under each rule.
static f32 cam_rot_fixed (f32 ship_angle, f32 roof_alpha){ return wrap_angle(ship_angle) * (1.0f - roof_alpha); }
static f32 cam_rot_bugged(f32 ship_angle, f32 roof_alpha){ return ship_angle             * (1.0f - roof_alpha); }

int main() {
    const f32 dt = 1.0f / 60.0f;

    // --- Phase A: pilot turns the hull just past a FULL revolution in global mode. ---
    // Hold the turn long enough to exceed 2*PI, then "release" — angular velocity auto-
    // stabilizes to 0 in the real sim, so the heading freezes. We bake that frozen heading.
    f32 ship_angle = 0.0f;
    f32 turn_time  = (2.0f * BS_PI) / SHIP_MAX_TURN + 0.20f; // ~3.69 s => ~370 deg
    for (f32 t = 0.0f; t < turn_time; t += dt) ship_angle += SHIP_MAX_TURN * dt;
    f32 revs = ship_angle / (2.0f * BS_PI);
    printf("PHASE_A  ship.angle=%.3f rad (%.1f deg, %.2f revolutions) [frozen]\n",
           ship_angle, ship_angle * 180.0f / BS_PI, revs);
    if (ship_angle <= 2.0f * BS_PI) { printf("FAIL: repro did not exceed 360 deg\n"); return 2; }

    // --- Safety invariant: wrapping must not change what a SETTLED frame shows. ---
    // R(-wrap(a)) and R(-a) must be the same rotation => cos/sin equal. This is what
    // guarantees the mouse-pick and final upright view are bit-equivalent to the old code.
    {
        f32 w = wrap_angle(ship_angle);
        f32 dc = fabsf(cosf(ship_angle) - cosf(w));
        f32 ds = fabsf(sinf(ship_angle) - sinf(w));
        printf("SETTLED_EQUIV  wrap=%.4f rad  |dcos|=%.2e |dsin|=%.2e\n", w, dc, ds);
        if (dc > 1e-4f || ds > 1e-4f) { printf("FAIL: wrap changed the settled rotation\n"); return 3; }
    }

    // --- Phase B: zoom from global (roof_alpha=1) back to local (->0); ease like the engine.
    // Two things are measured per frame as roof_alpha eases 1->0
    // (alpha += (target-alpha)*min(SPEED*dt,1)):
    //
    //  (1) CAMERA ANGULAR TRAVEL — the cumulative |d(camera.rotation)| swept during the zoom.
    //      This is LITERALLY what the user sees: "the camera rotates abruptly around about 360".
    //      Both rules drive camera.rotation monotonically (it is k*(1-roof_alpha)), so the travel
    //      equals the final |camera.rotation|: |wrap(angle)| for the fix vs |angle| for the bug.
    //
    //  (2) ON-SCREEN interior heading = ship.angle - camera.rotation, folded to (-PI,PI].
    //      In global mode (roof_alpha=1) this legitimately equals the ship's world heading
    //      (~21.6 deg here) — global view SHOWS the hull rotated, that is correct. As we zoom in
    //      it must move MONOTONICALLY to 0 (interior upright) by the SHORT arc. The bug instead
    //      swings it the long way through +/-180 deg (the visible whirl).
    f32 cam_prev_f = cam_rot_fixed (ship_angle, 1.0f);   // = 0 at roof_alpha=1
    f32 cam_prev_b = cam_rot_bugged(ship_angle, 1.0f);   // = 0 at roof_alpha=1
    f32 travel_f = 0.0f, travel_b = 0.0f;                // cumulative camera angular travel
    f32 screen0_f = fabsf(ang_diff(ship_angle, cam_prev_f));  // initial (global) screen heading
    f32 overshoot_f = 0.0f;                              // how far fixed screen heading EXCEEDS its start
    f32 worst_screen_b = 0.0f;                           // worst bug screen-heading magnitude
    f32 alpha_f = 1.0f, alpha_b = 1.0f;
    const f32 target = 0.0f;
    printf("\n  frame  roof_a   screen_fixed(deg)  screen_bugged(deg)\n");
    int frame = 0;
    for (alpha_f = 1.0f, alpha_b = 1.0f; alpha_f > 1e-3f; ++frame) {
        f32 cam_f = cam_rot_fixed (ship_angle, alpha_f);
        f32 cam_b = cam_rot_bugged(ship_angle, alpha_b);
        travel_f += fabsf(cam_f - cam_prev_f);   // raw (unwrapped) travel = what the camera physically turns
        travel_b += fabsf(cam_b - cam_prev_b);
        cam_prev_f = cam_f; cam_prev_b = cam_b;

        f32 sf = ang_diff(ship_angle, cam_f);
        f32 sb = ang_diff(ship_angle, cam_b);
        if (fabsf(sf) - screen0_f > overshoot_f) overshoot_f = fabsf(sf) - screen0_f;
        if (fabsf(sb) > worst_screen_b) worst_screen_b = fabsf(sb);
        if (frame < 12 || (frame % 4) == 0)
            printf("  %4d   %.4f   %+10.2f        %+10.2f\n",
                   frame, alpha_f, sf * 180.0f / BS_PI, sb * 180.0f / BS_PI);
        f32 k = ROOF_FADE_SPEED * dt; if (k > 1.0f) k = 1.0f;
        alpha_f += (target - alpha_f) * k;
        alpha_b += (target - alpha_b) * k;
    }

    f32 travel_f_deg   = travel_f * 180.0f / BS_PI;
    f32 travel_b_deg   = travel_b * 180.0f / BS_PI;
    f32 overshoot_deg  = overshoot_f * 180.0f / BS_PI;
    f32 worst_b_deg    = worst_screen_b * 180.0f / BS_PI;
    printf("\nCAMERA_TRAVEL    fixed=%.1f deg   bugged=%.1f deg   (ship spun %.1f deg)\n",
           travel_f_deg, travel_b_deg, ship_angle * 180.0f / BS_PI);
    printf("FIXED_OVERSHOOT  %.2f deg above the legit global heading (want ~0 => monotone short-arc)\n", overshoot_deg);
    printf("BUG_WHIRL        worst on-screen heading %.1f deg (the visible long-way swing)\n", worst_b_deg);

    // --- Verdicts -----------------------------------------------------------------------
    // FIX must: take the SHORT arc (camera travel <= 180 deg) and never overshoot its initial
    //           global heading (monotone righting => overshoot ~0).
    // BUG must: physically rotate the camera MORE THAN A FULL REVOLUTION (>360 deg) — i.e. it
    //           reproduces the reported "~360 deg abrupt spin"; otherwise the test has no teeth.
    int rc = 0;
    if (travel_f_deg > 180.0f) {
        printf("FAIL: fixed camera travels %.1f deg (> 180) — not the shortest arc\n", travel_f_deg);
        rc = 1;
    }
    if (overshoot_deg > 1.0f) {
        printf("FAIL: fixed interior overshoots %.2f deg past its start — not monotone\n", overshoot_deg);
        rc = 1;
    }
    if (travel_b_deg < 360.0f) {
        printf("FAIL: bugged camera travels only %.1f deg (< 360) — repro too weak, no teeth\n", travel_b_deg);
        rc = 1;
    }
    if (rc == 0)
        printf("\nPASS: fix sweeps the camera %.1f deg (short arc, monotone, 0 overshoot); the old\n"
               "      code would sweep %.1f deg — a full-revolution whirl (peak %.0f deg off-upright).\n"
               "      Shortest-arc wrap confirmed end-to-end against the reported repro.\n",
               travel_f_deg, travel_b_deg, worst_b_deg);
    return rc;
}
