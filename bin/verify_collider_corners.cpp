// =====================================================================================
// Focused micro-harness for ship_collider_corners (the debug-collider geometry).
// Links the REAL sandbox ship.cpp against engine.lib, loads the stock player ship, and
// asserts the 4 returned world corners match an INDEPENDENT oracle (ship_local_to_world
// of the tight OBB's local corners) at identity AND at a rotated+translated pose. Proves
// the on-screen collider outline rides the exact pose-correct box ships_collide tests.
//
// Build (from repo ROOT so engine.dll sits beside the .exe):
//   clang++ -std=c++17 -g -DBS_DEBUG -DBSIMPORT -Isandbox/source -Iengine/source \
//     bin/verify_collider_corners.cpp sandbox/source/ship.cpp \
//     -L bin -lengine.lib -o bin/verify_collider_corners.exe && ./bin/verify_collider_corners.exe
// =====================================================================================
#include "ship.h"
#include <math/math_utils.h>
#include <cstdio>
#include <cmath>

using namespace bs_math;

static int g_fail = 0;
static void check(const char* name, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_fail;
}
static bool vclose(Vec2 a, Vec2 b, f32 eps = 0.01f) {
    return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps;
}

// Independent oracle for the player ship's tight occupied OBB. From assets/ship.tmap
// (tile_size 32, size 13x17): occupied cols 1..11, rows 0..16 -> half_w=208, half_h=272,
// so local extent x in [-176,176], y in [-272,272], center {0,0}. These are the LOCAL
// corners; lifting them through ship_local_to_world is a path-independent check of the
// accessor (which builds them from ship_obb's center+axes instead).
static const Vec2 LOCAL_CORNERS[4] = {
    Vec2{ -176.0f, -272.0f }, // (minx, miny)
    Vec2{  176.0f, -272.0f }, // (maxx, miny)
    Vec2{  176.0f,  272.0f }, // (maxx, maxy)
    Vec2{ -176.0f,  272.0f }, // (minx, maxy)
};

static void run_pose(const char* label, Ship* s, Vec2 origin, f32 angle) {
    std::printf("\n-- %s: origin {%.1f,%.1f} angle %.3f --\n", label, origin.x, origin.y, angle);
    s->origin = origin;
    s->angle  = angle;

    Vec2 got[4];
    check("ship_collider_corners returns TRUE", ship_collider_corners(s, got));

    for (int i = 0; i < 4; ++i) {
        Vec2 want = ship_local_to_world(s, LOCAL_CORNERS[i]);
        char buf[64];
        std::snprintf(buf, sizeof buf, "corner[%d] matches ship_local_to_world oracle", i);
        check(buf, vclose(got[i], want));
    }

    // Edge lengths must equal the footprint dimensions at ANY pose (rigid box: 352 x 544).
    f32 w = vec2_length(vec2_sub(got[1], got[0]));
    f32 h = vec2_length(vec2_sub(got[3], got[0]));
    check("width edge == 352 (2*hx)",  std::fabs(w - 352.0f) < 0.05f);
    check("height edge == 544 (2*hy)", std::fabs(h - 544.0f) < 0.05f);

    // Opposite corners share the box center == ship origin (center_local is {0,0}).
    Vec2 mid02 = vec2_scale(vec2_add(got[0], got[2]), 0.5f);
    Vec2 mid13 = vec2_scale(vec2_add(got[1], got[3]), 0.5f);
    check("diagonals bisect at the ship origin", vclose(mid02, origin) && vclose(mid13, origin));
}

int main() {
    Ship player{};
    if (!ship_load(&player, "assets/ship.tmap")) { std::printf("FATAL: load player\n"); return 2; }

    // Null-safety contract.
    Vec2 dummy[4];
    check("null ship -> FALSE", !ship_collider_corners(nullptr, dummy));
    check("null out      -> FALSE", !ship_collider_corners(&player, nullptr));

    run_pose("identity pose", &player, Vec2{ 0.0f, 0.0f }, 0.0f);
    run_pose("translated + rotated", &player, Vec2{ 120.0f, -45.0f }, 0.7853981634f); // +45 deg

    std::printf("\n==== %s ====\n", g_fail == 0 ? "ALL COLLIDER-CORNER CHECKS PASSED" : "COLLIDER-CORNER CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
