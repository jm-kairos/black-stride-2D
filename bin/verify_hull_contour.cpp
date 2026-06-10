// =====================================================================================
// Headless micro-harness for the SINGLE-PASS marching-squares hull contour
// (ship_hull_contour). Links the REAL sandbox hull_contour.cpp + ship.cpp against
// engine.lib. Asserts against HAND-DERIVED oracles on small fixtures (single tile traced
// segment-by-segment; 3x3 octagon area 900 - 4*12.5 = 850), the chamfer area invariant
//   sum(signed_area) = occupied*ts^2 - chi*ts^2/2      (chi from an INDEPENDENT flood fill),
// a midpoint-lattice structural check (every vertex on a tile-edge midpoint), winding signs,
// and pose-independence on the stock player ship.
//
// Build (from repo ROOT so engine.dll sits beside the .exe):
//   clang++ -std=c++17 -g -DBS_DEBUG -DBSIMPORT -Isandbox/source -Iengine/source \
//     bin/verify_hull_contour.cpp sandbox/source/hull_contour.cpp sandbox/source/ship.cpp \
//     -L bin -lengine.lib -o bin/verify_hull_contour.exe && ./bin/verify_hull_contour.exe
// =====================================================================================
#include "hull_contour.h"
#include "ship.h"
#include <math/math_utils.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <vector>

using namespace bs_math;

static int g_fail = 0;
static void check(const char* name, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_fail;
}
static bool fclose_(f32 a, f32 b, f32 eps = 0.05f) { return std::fabs(a - b) < eps; }

static void build_fixture(Ship* s, i32 cols, i32 rows, f32 ts, const char* const* pattern) {
    std::memset(s, 0, sizeof(*s));
    s->cols = cols; s->rows = rows; s->tile_size = ts;
    s->origin = Vec2{ 0.0f, 0.0f }; s->angle = 0.0f;
    for (i32 r = 0; r < rows; ++r)
        for (i32 c = 0; c < cols; ++c)
            s->tiles[r * cols + c] = (pattern[r][c] == '#') ? TILE_HULL : TILE_EMPTY;
}

static f32 total_signed_area(const HullContour* hc) {
    f32 sum = 0.0f;
    for (i32 i = 0; i < (i32)hc->loop_len.size(); ++i) sum += hull_loop_signed_area(hc, i);
    return sum;
}
static i32 count_occupied(const Ship* s) {
    i32 n = 0;
    for (i32 r = 0; r < s->rows; ++r)
        for (i32 c = 0; c < s->cols; ++c)
            if (ship_tile_is_structure(s, c, r)) ++n;
    return n;
}

// ---- Independent Euler characteristic via flood fill (oracle, NOT using the contour) -------
// Occupied uses 4-connectivity (matches the separating-saddle convention); holes = empty
// regions (4-conn) that don't reach the padded border. chi = components - holes.
static i32 euler_chi(const Ship* s) {
    const i32 C = s->cols, R = s->rows;
    auto occ = [&](i32 c, i32 r) { return ship_tile_is_structure(s, c, r) ? 1 : 0; };
    std::vector<int> seen(C * R, 0);
    auto flood = [&](i32 c0, i32 r0, int want) {
        std::vector<std::pair<i32,i32>> st{ {c0, r0} };
        seen[r0 * C + c0] = 1;
        while (!st.empty()) {
            auto [c, r] = st.back(); st.pop_back();
            const i32 dc[4] = { 1,-1,0,0 }, dr[4] = { 0,0,1,-1 };
            for (i32 k = 0; k < 4; ++k) {
                i32 nc = c + dc[k], nr = r + dr[k];
                if (nc < 0 || nr < 0 || nc >= C || nr >= R) continue;
                if (seen[nr * C + nc]) continue;
                if (occ(nc, nr) != want) continue;
                seen[nr * C + nc] = 1; st.push_back({ nc, nr });
            }
        }
    };
    i32 comps = 0;
    for (i32 r = 0; r < R; ++r) for (i32 c = 0; c < C; ++c)
        if (occ(c, r) == 1 && !seen[r * C + c]) { ++comps; flood(c, r, 1); }
    // Holes: empty regions not touching the border. Mark border-connected empties first.
    std::vector<int> bseen(C * R, 0);
    std::vector<std::pair<i32,i32>> st;
    for (i32 c = 0; c < C; ++c) {
        if (occ(c, 0) == 0 && !bseen[c]) { bseen[c] = 1; st.push_back({ c, 0 }); }
        if (occ(c, R - 1) == 0 && !bseen[(R-1)*C + c]) { bseen[(R-1)*C + c] = 1; st.push_back({ c, R-1 }); }
    }
    for (i32 r = 0; r < R; ++r) {
        if (occ(0, r) == 0 && !bseen[r*C]) { bseen[r*C] = 1; st.push_back({ 0, r }); }
        if (occ(C-1, r) == 0 && !bseen[r*C + C-1]) { bseen[r*C + C-1] = 1; st.push_back({ C-1, r }); }
    }
    while (!st.empty()) {
        auto [c, r] = st.back(); st.pop_back();
        const i32 dc[4] = { 1,-1,0,0 }, dr[4] = { 0,0,1,-1 };
        for (i32 k = 0; k < 4; ++k) {
            i32 nc = c + dc[k], nr = r + dr[k];
            if (nc < 0 || nr < 0 || nc >= C || nr >= R) continue;
            if (bseen[nr * C + nc] || occ(nc, nr) != 0) continue;
            bseen[nr * C + nc] = 1; st.push_back({ nc, nr });
        }
    }
    std::fill(seen.begin(), seen.end(), 0);
    i32 holes = 0;
    for (i32 r = 0; r < R; ++r) for (i32 c = 0; c < C; ++c)
        if (occ(c, r) == 0 && !bseen[r*C + c] && !seen[r*C + c]) { ++holes; flood(c, r, 0); }
    return comps - holes;
}

// Every contour vertex must sit on a tile-edge MIDPOINT: in doubled tile-center coords
// x2 = 2*(x+half_w)/ts - 1 and y2 = 2*(half_h-y)/ts - 1 are integers with EXACTLY ONE odd.
static bool all_verts_on_midpoint_lattice(const Ship* s, const HullContour* hc) {
    const f32 half_w = s->cols * s->tile_size * 0.5f;
    const f32 half_h = s->rows * s->tile_size * 0.5f;
    for (const Vec2& v : hc->verts) {
        f32 fx2 = 2.0f * (v.x + half_w) / s->tile_size - 1.0f;
        f32 fy2 = 2.0f * (half_h - v.y) / s->tile_size - 1.0f;
        i32 x2 = (i32)std::lround(fx2), y2 = (i32)std::lround(fy2);
        if (!fclose_((f32)x2, fx2, 0.01f) || !fclose_((f32)y2, fy2, 0.01f)) return false;
        if (((x2 & 1) != 0) == ((y2 & 1) != 0)) return false; // need exactly one odd
    }
    return true;
}
static bool has_vert(const HullContour* hc, Vec2 p) {
    for (const Vec2& v : hc->verts) if (fclose_(v.x, p.x) && fclose_(v.y, p.y)) return true;
    return false;
}

int main() {
    HullContour hc;
    Ship dummy{};
    check("null ship -> FALSE", !ship_hull_contour(nullptr, &hc));
    check("null out  -> FALSE", !ship_hull_contour(&dummy, nullptr));

    const f32 TS = 10.0f;

    // ---- Fixture S: single tile -> diamond, 4 verts at (+-5,0)/(0,+-5), area +50 ----
    {
        const char* pat[] = { "#" };
        Ship s; build_fixture(&s, 1, 1, TS, pat);
        std::printf("\n-- Fixture S: single tile (diamond) --\n");
        check("returns TRUE", ship_hull_contour(&s, &hc));
        check("1 loop", hc.loop_len.size() == 1);
        check("4 chamfer verts", hc.loop_len.size() == 1 && hc.loop_len[0] == 4);
        check("area == +50 (100 - chi*50)", fclose_(total_signed_area(&hc), 50.0f));
        check("verts are the diamond {(+-5,0),(0,+-5)}",
              has_vert(&hc, {5,0}) && has_vert(&hc, {-5,0}) && has_vert(&hc, {0,5}) && has_vert(&hc, {0,-5}));
        check("CCW (area > 0)", hull_loop_signed_area(&hc, 0) > 0.0f);
        check("verts on midpoint lattice", all_verts_on_midpoint_lattice(&s, &hc));
    }

    // ---- Fixture A: 3x3 solid -> octagon, 8 verts, area 900 - 4*12.5 = 850 ----
    {
        const char* pat[] = { "###", "###", "###" };
        Ship s; build_fixture(&s, 3, 3, TS, pat);
        std::printf("\n-- Fixture A: 3x3 solid (octagon) --\n");
        check("returns TRUE", ship_hull_contour(&s, &hc));
        check("1 loop", hc.loop_len.size() == 1);
        check("8 chamfered verts", hc.loop_len.size() == 1 && hc.loop_len[0] == 8);
        check("area == +850 (900 - chi*50)", fclose_(total_signed_area(&hc), 850.0f));
        check("CCW (area > 0)", hull_loop_signed_area(&hc, 0) > 0.0f);
        check("verts on midpoint lattice", all_verts_on_midpoint_lattice(&s, &hc));
    }

    // ---- Fixture B: L-tromino -> 1 loop, area 300 - chi*50 = 250 ----
    {
        const char* pat[] = { "#.", "##" };
        Ship s; build_fixture(&s, 2, 2, TS, pat);
        std::printf("\n-- Fixture B: L-tromino --\n");
        check("returns TRUE", ship_hull_contour(&s, &hc));
        check("1 loop", hc.loop_len.size() == 1);
        check("area == +250 (300 - chi*50)", fclose_(total_signed_area(&hc), 250.0f));
        check("verts on midpoint lattice", all_verts_on_midpoint_lattice(&s, &hc));
    }

    // ---- Fixture C: 3x3 ring (center hole) -> 2 loops, chi=0, area 800 ----
    {
        const char* pat[] = { "###", "#.#", "###" };
        Ship s; build_fixture(&s, 3, 3, TS, pat);
        std::printf("\n-- Fixture C: 3x3 ring with hole --\n");
        check("returns TRUE", ship_hull_contour(&s, &hc));
        check("2 loops (outer + hole)", hc.loop_len.size() == 2);
        check("area == +800 (800 - chi*50, chi=0)", fclose_(total_signed_area(&hc), 800.0f));
        f32 a0 = hull_loop_signed_area(&hc, 0), a1 = (hc.loop_len.size() > 1) ? hull_loop_signed_area(&hc, 1) : 0.0f;
        check("one CCW outer + one CW hole", (a0 > 0.0f) != (a1 > 0.0f));
        check("verts on midpoint lattice", all_verts_on_midpoint_lattice(&s, &hc));
    }

    // ---- Fixture D: diagonal pair -> saddle SEPARATES into 2 diamonds, chi=2, area 100 ----
    {
        const char* pat[] = { "#.", ".#" };
        Ship s; build_fixture(&s, 2, 2, TS, pat);
        std::printf("\n-- Fixture D: diagonal pinch (separated) --\n");
        check("returns TRUE", ship_hull_contour(&s, &hc));
        check("2 separate loops at saddle", hc.loop_len.size() == 2);
        check("area == +100 (200 - chi*50, chi=2)", fclose_(total_signed_area(&hc), 100.0f));
        check("both loops CCW (two diamonds)",
              hull_loop_signed_area(&hc, 0) > 0.0f && hull_loop_signed_area(&hc, 1) > 0.0f);
        check("verts on midpoint lattice", all_verts_on_midpoint_lattice(&s, &hc));
    }

    // ---- Stock player ship: chamfer invariant w/ INDEPENDENT chi, lattice, pose-independence ----
    {
        Ship s{};
        if (!ship_load(&s, "assets/ship.tmap")) { std::printf("FATAL: load player\n"); return 2; }
        std::printf("\n-- Stock player ship (assets/ship.tmap) --\n");
        check("returns TRUE", ship_hull_contour(&s, &hc));
        check("at least 1 loop", hc.loop_len.size() >= 1);

        i32 occ = count_occupied(&s);
        i32 chi = euler_chi(&s);
        f32 want = (f32)occ * s.tile_size * s.tile_size - (f32)chi * s.tile_size * s.tile_size * 0.5f;
        std::printf("     occupied=%d  chi=%d  predicted_area=%.1f  got=%.1f\n",
                    occ, chi, want, total_signed_area(&hc));
        check("area == occupied*ts^2 - chi*ts^2/2 (independent chi)", fclose_(total_signed_area(&hc), want, 1.0f));
        check("all verts on midpoint lattice", all_verts_on_midpoint_lattice(&s, &hc));

        HullContour before = hc;
        s.origin = Vec2{ 1234.0f, -567.0f };
        s.angle  = 0.9f;
        HullContour after;
        ship_hull_contour(&s, &after);
        bool same = (before.verts.size() == after.verts.size()) &&
                    (before.loop_len.size() == after.loop_len.size());
        for (i32 i = 0; same && i < (i32)before.verts.size(); ++i)
            same = fclose_(before.verts[i].x, after.verts[i].x) && fclose_(before.verts[i].y, after.verts[i].y);
        check("pose-independent (verts identical after origin/angle change)", same);
    }

    std::printf("\n==== %s ====\n", g_fail == 0 ? "ALL HULL-CONTOUR CHECKS PASSED" : "HULL-CONTOUR CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
