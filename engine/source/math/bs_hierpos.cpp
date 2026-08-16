#include "math/bs_hierpos.h"
#include <math.h>

namespace bs_math
{
    // Internal: canonicalize from f64 world coordinates.
    static HierPos2 hierpos_from_f64(f64 wx, f64 wy, f32 cell_size) {
        f64 cs = (f64)cell_size;
        f64 half = cs * 0.5;

        i64 cx = (i64)floor(wx / cs);
        i64 cy = (i64)floor(wy / cs);

        f64 lx = wx - (f64)cx * cs;
        f64 ly = wy - (f64)cy * cs;

        if (lx >= half) { cx += 1; lx -= cs; }
        if (ly >= half) { cy += 1; ly -= cs; }

        HierPos2 hp;
        hp.cell = { cx, cy };
        hp.local = { (f32)lx, (f32)ly };
        return hp;
    }

    HierPos2 hierpos_from_vec2(Vec2 world, f32 cell_size) {
        return hierpos_from_f64((f64)world.x, (f64)world.y, cell_size);
    }

    Vec2 hierpos_to_vec2(const HierPos2* hp, f32 cell_size) {
        return Vec2{
            (f32)hp->cell.x * cell_size + hp->local.x,
            (f32)hp->cell.y * cell_size + hp->local.y
        };
    }

    void hierpos_to_f64(const HierPos2* hp, f32 cell_size, f64* out_x, f64* out_y) {
        f64 cs = (f64)cell_size;
        if (out_x) *out_x = (f64)hp->cell.x * cs + (f64)hp->local.x;
        if (out_y) *out_y = (f64)hp->cell.y * cs + (f64)hp->local.y;
    }

    HierPos2 hierpos_normalize(HierPos2 hp, f32 cell_size) {
        return hierpos_from_f64(
            (f64)hp.cell.x * (f64)cell_size + (f64)hp.local.x,
            (f64)hp.cell.y * (f64)cell_size + (f64)hp.local.y,
            cell_size
        );
    }

    HierPos2 hierpos_lerp(const HierPos2* a, const HierPos2* b, f32 t, f32 cell_size) {
        f64 ax, ay, bx, by;
        hierpos_to_f64(a, cell_size, &ax, &ay);
        hierpos_to_f64(b, cell_size, &bx, &by);

        f64 tx = (f64)t;
        f64 rx = ax + tx * (bx - ax);
        f64 ry = ay + tx * (by - ay);

        return hierpos_from_f64(rx, ry, cell_size);
    }

    HierPos2 hierpos_add_f64(const HierPos2* hp, f64 dx, f64 dy, f32 cell_size) {
        f64 x, y;
        hierpos_to_f64(hp, cell_size, &x, &y);
        return hierpos_from_f64(x + dx, y + dy, cell_size);
    }

    Vec2 hierpos_diff(const HierPos2* a, const HierPos2* b, f32 cell_size) {
        f64 ax, ay, bx, by;
        hierpos_to_f64(a, cell_size, &ax, &ay);
        hierpos_to_f64(b, cell_size, &bx, &by);
        return Vec2{ (f32)(ax - bx), (f32)(ay - by) };
    }

    b8 bs_hierpos_selftest(void) {
        const f32 cs = BS_HIERPOS_CELL_SIZE;
        const f32 half = BS_HIERPOS_HALF_CELL;
        const f32 eps = 0.001f;

        struct TestCase {
            f64 wx, wy;
            i64 ex, ey;
            f32 elx, ely;
        };

        TestCase cases[] = {
            {  0.0,           0.0,           0,  0,  0.0f,          0.0f          },
            {  half - eps,    0.0,           0,  0,  half - eps,    0.0f          },
            {  half,          0.0,           1,  0, -half,          0.0f          },
            {  cs,            0.0,           1,  0,  0.0f,          0.0f          },
            { -half + eps,    0.0,           0,  0, -half + eps,    0.0f          },
            { -half,          0.0,           0,  0, -half,          0.0f          },
            { -half - eps,    0.0,          -1,  0,  half - eps,    0.0f          },
            { -cs,            0.0,          -1,  0,  0.0f,          0.0f          },
            {  cs * 10.0,     cs * 5.0 + half, 10, 6, 0.0f,          -half          },
            { -cs * 3.0 - half, -cs * 2.0,    -3, -2, -half,          0.0f          },
        };

        for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
            const TestCase& tc = cases[i];
            HierPos2 hp = hierpos_from_f64(tc.wx, tc.wy, cs);

            if (hp.cell.x != tc.ex || hp.cell.y != tc.ey) {
                // Test failed
                return FALSE;
            }

            f32 dx = hp.local.x - tc.elx;
            f32 dy = hp.local.y - tc.ely;
            if (dx * dx > eps * eps || dy * dy > eps * eps) {
                return FALSE;
            }

            // Round-trip: hp -> f64 -> hp2, should match.
            f64 rx, ry;
            hierpos_to_f64(&hp, cs, &rx, &ry);
            if (fabs(rx - tc.wx) > (f64)eps || fabs(ry - tc.wy) > (f64)eps) {
                return FALSE;
            }
        }

        // Lerp monotonicity test across a cell boundary.
        HierPos2 a = hierpos_from_f64(-half + eps, 0.0, cs);  // cell 0, near boundary
        HierPos2 b = hierpos_from_f64( half + eps, 0.0, cs);  // cell 1, past boundary

        f64 prev_x = -1e300;
        for (f32 t = 0.0f; t <= 1.0f; t += 0.1f) {
            HierPos2 mid = hierpos_lerp(&a, &b, t, cs);
            f64 mx, my;
            hierpos_to_f64(&mid, cs, &mx, &my);
            if (mx < prev_x - (f64)eps) {
                return FALSE; // non-monotonic
            }
            prev_x = mx;
        }

        // ---- Precision-critical: large-distance lerp (travel-scale, ~50k units) ----
        // This is the exact scenario used by the travel system.
        HierPos2 travel_origin = hierpos_from_vec2(Vec2{ 0.0f, 0.0f }, cs);
        HierPos2 travel_dest   = hierpos_from_vec2(Vec2{ 50000.0f, 0.0f }, cs);

        f64 origin_x, origin_y;
        hierpos_to_f64(&travel_origin, cs, &origin_x, &origin_y);

        // Midpoint at t=0.5 should be exactly 25,000 units from origin.
        HierPos2 mid_travel = hierpos_lerp(&travel_origin, &travel_dest, 0.5f, cs);
        f64 mid_x, mid_y;
        hierpos_to_f64(&mid_travel, cs, &mid_x, &mid_y);
        f64 mid_err = fabs(mid_x - 25000.0);
        if (mid_err > (f64)eps) {
            return FALSE; // midpoint precision lost
        }

        // Verify every 10% step on the 50k path advances monotonically.
        f64 travel_prev = origin_x;
        for (f32 t = 0.0f; t <= 1.0f; t += 0.1f) {
            HierPos2 step = hierpos_lerp(&travel_origin, &travel_dest, t, cs);
            f64 sx, sy;
            hierpos_to_f64(&step, cs, &sx, &sy);
            if (sx < travel_prev - (f64)eps) {
                return FALSE; // travel lerp non-monotonic
            }
            travel_prev = sx;
        }

        // ---- add_f64 precision: add a tiny delta to a far-cell position ----
        HierPos2 far_pos = hierpos_from_f64(cs * 100.0 + 1.0, cs * 50.0 + 2.0, cs);
        HierPos2 far_moved = hierpos_add_f64(&far_pos, 0.123456789, -0.987654321, cs);
        f64 far_mx, far_my;
        hierpos_to_f64(&far_moved, cs, &far_mx, &far_my);
        f64 add_err_x = fabs(far_mx - (cs * 100.0 + 1.0 + 0.123456789));
        f64 add_err_y = fabs(far_my - (cs * 50.0 + 2.0 - 0.987654321));
        if (add_err_x > (f64)eps || add_err_y > (f64)eps) {
            return FALSE; // add_f64 lost sub-cell precision
        }

        // ---- normalize round-trip: deliberately de-normalize and recover ----
        HierPos2 denorm;
        denorm.cell = { 3, -7 };
        denorm.local = Vec2{ half + 100.0f, -half - 200.0f }; // local outside canonical range
        HierPos2 recov = hierpos_normalize(denorm, cs);
        f64 rec_x, rec_y;
        hierpos_to_f64(&recov, cs, &rec_x, &rec_y);
        f64 orig_x = 3.0 * cs + (half + 100.0f);
        f64 orig_y = -7.0 * cs + (-half - 200.0f);
        if (fabs(rec_x - orig_x) > (f64)eps || fabs(rec_y - orig_y) > (f64)eps) {
            return FALSE; // normalize lost the world position
        }

        return TRUE;
    }
}
