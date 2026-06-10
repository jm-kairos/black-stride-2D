// Single-pass hull-contour smoothing via Marching Squares. See hull_contour.h for the contract.
//
// Samples tile occupancy (ship_tile_is_structure) at tile centers; each 2x2 sample neighbourhood
// ("ms-cell") emits 0/1/2 directed unit segments between tile-edge MIDPOINTS via a 16-case
// oriented table (occupied kept on the LEFT of travel). Saddles (cases 5/10) separate the two
// diagonally-occupied corners. Segments stitch head-to-tail into closed loops; collinear interior
// vertices are dropped. Output is ship-LOCAL corner loops, pose-independent. Pure geometry.

#include "hull_contour.h"

#include <map>
#include <vector>
#include <cmath>   // sqrtf, fabsf

using namespace bs_math;

namespace {

// Midpoint corner tags of an ms-cell.
enum MidTag { MID_T = 0, MID_B = 1, MID_L = 2, MID_R = 3 };

// One directed boundary segment between two midpoint-vertex ids.
struct Seg { i32 from; i32 to; };

// Doubled integer coords of an ms-cell's edge midpoints (so every midpoint is an integer pair;
// exactly one coordinate is odd). cell (gx,gy): T=(2gx+1,2gy) B=(2gx+1,2gy+2) L=(2gx,2gy+1)
// R=(2gx+2,2gy+1). row/y2 increases DOWNWARD (matches the tilemap; converted to y-up at the end).
inline void mid_coords(i32 gx, i32 gy, MidTag m, i32* x2, i32* y2) {
    switch (m) {
        case MID_T: *x2 = 2*gx + 1; *y2 = 2*gy;     break;
        case MID_B: *x2 = 2*gx + 1; *y2 = 2*gy + 2; break;
        case MID_L: *x2 = 2*gx;     *y2 = 2*gy + 1; break;
        case MID_R: *x2 = 2*gx + 2; *y2 = 2*gy + 1; break;
    }
}

// 16-case oriented segment table. Each case lists 0/1/2 segments as (from,to) midpoint tags,
// occupied-on-LEFT (=> outer loops CCW, holes CW). Saddles 5 & 10 separate occupied corners.
// mask bit layout: TL=1, TR=2, BR=4, BL=8.
struct CaseSegs { i32 count; MidTag seg[2][2]; };
const CaseSegs kCases[16] = {
    /* 0  ....  */ { 0, {{MID_T,MID_T},{MID_T,MID_T}} },
    /* 1  TL    */ { 1, {{MID_L,MID_T},{MID_T,MID_T}} },
    /* 2  TR    */ { 1, {{MID_T,MID_R},{MID_T,MID_T}} },
    /* 3  TL TR */ { 1, {{MID_L,MID_R},{MID_T,MID_T}} },
    /* 4  BR    */ { 1, {{MID_R,MID_B},{MID_T,MID_T}} },
    /* 5  TL BR */ { 2, {{MID_L,MID_T},{MID_R,MID_B}} }, // saddle: separate
    /* 6  TR BR */ { 1, {{MID_T,MID_B},{MID_T,MID_T}} },
    /* 7  ~BL   */ { 1, {{MID_L,MID_B},{MID_T,MID_T}} },
    /* 8  BL    */ { 1, {{MID_B,MID_L},{MID_T,MID_T}} },
    /* 9  TL BL */ { 1, {{MID_B,MID_T},{MID_T,MID_T}} },
    /* 10 TR BL */ { 2, {{MID_T,MID_R},{MID_B,MID_L}} }, // saddle: separate
    /* 11 ~BR   */ { 1, {{MID_B,MID_R},{MID_T,MID_T}} },
    /* 12 BR BL */ { 1, {{MID_R,MID_L},{MID_T,MID_T}} },
    /* 13 ~TR   */ { 1, {{MID_R,MID_T},{MID_T,MID_T}} },
    /* 14 ~TL   */ { 1, {{MID_T,MID_L},{MID_T,MID_T}} },
    /* 15 ####  */ { 0, {{MID_T,MID_T},{MID_T,MID_T}} },
};

inline Vec2 norm2(Vec2 v) {
    f32 L = sqrtf(v.x*v.x + v.y*v.y);
    return (L > 1.0e-12f) ? Vec2{ v.x / L, v.y / L } : Vec2{ 0.0f, 0.0f };
}

} // namespace

b8 ship_hull_contour(const Ship* ship, HullContour* out) {
    if (!ship || !out) return FALSE;
    out->verts.clear();
    out->loop_start.clear();
    out->loop_len.clear();

    const i32 cols = ship->cols;
    const i32 rows = ship->rows;
    const f32 ts     = ship->tile_size;
    const f32 half_w = cols * ts * 0.5f;
    const f32 half_h = rows * ts * 0.5f;

    // Dedupe midpoints by doubled-int key -> vertex id; remember each vertex's ship-local pos.
    std::map<long long, i32> vid_of;
    std::vector<Vec2>        vpos;
    auto vert_id = [&](i32 x2, i32 y2) -> i32 {
        long long key = (long long)y2 * 4000003LL + (long long)x2;
        auto it = vid_of.find(key);
        if (it != vid_of.end()) return it->second;
        // doubled coords -> tile-center coords (fc,fr) -> ship-local (y-up), matching
        // ship_tile_center_local: x = -half_w + (fc+0.5)*ts,  y = half_h - (fr+0.5)*ts.
        f32 fc = (f32)x2 * 0.5f;
        f32 fr = (f32)y2 * 0.5f;
        i32 id = (i32)vpos.size();
        vpos.push_back(Vec2{ -half_w + (fc + 0.5f) * ts, half_h - (fr + 0.5f) * ts });
        vid_of.emplace(key, id);
        return id;
    };

    // ---- 1+2. Walk ms-cells; emit oriented segments from the case table. -----------------
    std::vector<Seg> segs;
    for (i32 gy = -1; gy < rows; ++gy) {
        for (i32 gx = -1; gx < cols; ++gx) {
            i32 tl = ship_tile_is_structure(ship, gx,     gy)     ? 1 : 0;
            i32 tr = ship_tile_is_structure(ship, gx + 1, gy)     ? 1 : 0;
            i32 bl = ship_tile_is_structure(ship, gx,     gy + 1) ? 1 : 0;
            i32 br = ship_tile_is_structure(ship, gx + 1, gy + 1) ? 1 : 0;
            i32 mask = tl * 1 + tr * 2 + br * 4 + bl * 8;
            const CaseSegs& cs = kCases[mask];
            for (i32 k = 0; k < cs.count; ++k) {
                i32 fx, fy, tx, ty;
                mid_coords(gx, gy, cs.seg[k][0], &fx, &fy);
                mid_coords(gx, gy, cs.seg[k][1], &tx, &ty);
                segs.push_back({ vert_id(fx, fy), vert_id(tx, ty) });
            }
        }
    }
    if (segs.empty()) return FALSE; // no occupied tiles -> no boundary

    // Outgoing adjacency by from-vertex (in/out degree is 1 except saddles use distinct mids).
    std::vector<std::vector<i32>> out_segs(vpos.size());
    for (i32 e = 0; e < (i32)segs.size(); ++e) out_segs[segs[e].from].push_back(e);
    std::vector<bool> used(segs.size(), false);

    // ---- 3. Stitch into closed loops, then drop collinear interior vertices. --------------
    const i32 max_steps = (i32)segs.size() + 1;
    for (i32 e0 = 0; e0 < (i32)segs.size(); ++e0) {
        if (used[e0]) continue;

        std::vector<i32> loop_vids;
        i32 start_v = segs[e0].from;
        i32 cur     = e0;
        i32 steps   = 0;
        for (;;) {
            used[cur] = true;
            loop_vids.push_back(segs[cur].from);
            i32 nb = segs[cur].to;
            if (nb == start_v) break;
            if (++steps > max_steps) break; // safety on a malformed grid
            i32 nxt = -1;
            for (i32 cand : out_segs[nb]) { if (!used[cand]) { nxt = cand; break; } }
            if (nxt < 0) break;
            cur = nxt;
        }

        const i32 n = (i32)loop_vids.size();
        if (n < 3) continue;
        std::vector<Vec2> pts;
        pts.reserve(n);
        for (i32 id : loop_vids) pts.push_back(vpos[id]);

        std::vector<Vec2> merged;
        merged.reserve(n);
        for (i32 i = 0; i < n; ++i) {
            Vec2 p = pts[(i - 1 + n) % n];
            Vec2 c = pts[i];
            Vec2 q = pts[(i + 1) % n];
            Vec2 d1 = norm2(Vec2{ c.x - p.x, c.y - p.y });
            Vec2 d2 = norm2(Vec2{ q.x - c.x, q.y - c.y });
            f32 cross = d1.x * d2.y - d1.y * d2.x;
            if (fabsf(cross) > 1.0e-4f) merged.push_back(c); // real turn => corner; else collinear
        }
        if ((i32)merged.size() < 3) continue;

        out->loop_start.push_back((i32)out->verts.size());
        out->loop_len.push_back((i32)merged.size());
        for (const Vec2& v : merged) out->verts.push_back(v);
    }

    return out->loop_len.empty() ? FALSE : TRUE;
}

f32 hull_loop_signed_area(const HullContour* c, i32 loop) {
    if (!c || loop < 0 || loop >= (i32)c->loop_len.size()) return 0.0f;
    i32 s = c->loop_start[loop];
    i32 n = c->loop_len[loop];
    if (n < 3) return 0.0f;
    f32 area2 = 0.0f; // twice the shoelace signed area, y-up: positive => CCW
    for (i32 i = 0; i < n; ++i) {
        const Vec2& a = c->verts[s + i];
        const Vec2& b = c->verts[s + (i + 1) % n];
        area2 += a.x * b.y - b.x * a.y;
    }
    return area2 * 0.5f;
}
