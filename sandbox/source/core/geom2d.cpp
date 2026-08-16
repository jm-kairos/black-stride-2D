#include "core/geom2d.h"
#include <math.h>

using namespace bs_math;

// Even-odd ray-cast point-in-polygon test. `verts` is a closed polygon in world space.
b8 point_in_polygon(Vec2 p, const Vec2* verts, i32 n) {
    if (n < 3) return FALSE;
    b8 inside = FALSE;
    for (i32 i = 0, j = n - 1; i < n; j = i++) {
        b8 crosses = ((verts[i].y > p.y) != (verts[j].y > p.y)) &&
                     (p.x < (verts[j].x - verts[i].x) * (p.y - verts[i].y) /
                            (verts[j].y - verts[i].y) + verts[i].x);
        if (crosses) inside = !inside;
    }
    return inside;
}

// Distance from a point to a line segment [a, b].
f32 point_to_segment(Vec2 p, Vec2 a, Vec2 b) {
    Vec2 ab = vec2_sub(b, a);
    Vec2 ap = vec2_sub(p, a);
    f32 ab2 = ab.x * ab.x + ab.y * ab.y;
    if (ab2 < 0.0001f) return sqrtf(ap.x * ap.x + ap.y * ap.y);
    f32 t = clampf((ap.x * ab.x + ap.y * ab.y) / ab2, 0.0f, 1.0f);
    Vec2 closest = vec2_add(a, vec2_scale(ab, t));
    Vec2 d = vec2_sub(p, closest);
    return sqrtf(d.x * d.x + d.y * d.y);
}
