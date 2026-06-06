#include "math_utils.h"

#include <math.h>

namespace bs_math
{
    u32 clamp(u32 d, u32 min, u32 max) {
        const u32 t = d < min ? min : d;
        return (t > max ? max : t);
    }

    f32 clampf(f32 d, f32 min, f32 max) {
        const f32 t = d < min ? min : d;
        return (t > max ? max : t);
    }

    // ---- Vec2 ops ----
    Vec2 vec2_add(Vec2 a, Vec2 b) { return Vec2{ a.x + b.x, a.y + b.y }; }
    Vec2 vec2_sub(Vec2 a, Vec2 b) { return Vec2{ a.x - b.x, a.y - b.y }; }
    Vec2 vec2_scale(Vec2 a, f32 s) { return Vec2{ a.x * s, a.y * s }; }
    f32  vec2_dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }

    f32 vec2_length(Vec2 a) { return sqrtf(a.x * a.x + a.y * a.y); }

    Vec2 vec2_normalized(Vec2 a) {
        f32 len = vec2_length(a);
        if (len <= 0.0f) return Vec2{ 0.0f, 0.0f };
        f32 inv = 1.0f / len;
        return Vec2{ a.x * inv, a.y * inv };
    }

    Vec2 vec2_rotate(Vec2 a, f32 radians) {
        f32 c = cosf(radians);
        f32 s = sinf(radians);
        return Vec2{ a.x * c - a.y * s, a.x * s + a.y * c };
    }

    // ---- Vec3 ops ----
    Vec3 vec3_add(Vec3 a, Vec3 b) { return Vec3{ a.x + b.x, a.y + b.y, a.z + b.z }; }
    Vec3 vec3_sub(Vec3 a, Vec3 b) { return Vec3{ a.x - b.x, a.y - b.y, a.z - b.z }; }
    Vec3 vec3_scale(Vec3 a, f32 s) { return Vec3{ a.x * s, a.y * s, a.z * s }; }

    // ---- Mat4 ops ----
    // Storage is column-major: data[col * 4 + row].
    Mat4 mat4_identity() {
        Mat4 m = {};
        m.data[0]  = 1.0f;
        m.data[5]  = 1.0f;
        m.data[10] = 1.0f;
        m.data[15] = 1.0f;
        return m;
    }

    Mat4 mat4_mul(Mat4 a, Mat4 b) {
        // Column-major: result[col][row] = sum_k a[k][row] * b[col][k]
        Mat4 r = {};
        for (i32 col = 0; col < 4; ++col) {
            for (i32 row = 0; row < 4; ++row) {
                f32 sum = 0.0f;
                for (i32 k = 0; k < 4; ++k) {
                    sum += a.data[k * 4 + row] * b.data[col * 4 + k];
                }
                r.data[col * 4 + row] = sum;
            }
        }
        return r;
    }

    Mat4 mat4_ortho(f32 left, f32 right, f32 bottom, f32 top, f32 near_clip, f32 far_clip) {
        // Vulkan/SDL-GPU clip space: z in [0,1]. Column-major.
        Mat4 m = mat4_identity();
        f32 rl = right - left;
        f32 tb = top - bottom;
        f32 fn = far_clip - near_clip;

        m.data[0]  =  2.0f / rl;
        m.data[5]  =  2.0f / tb;
        m.data[10] = -1.0f / fn;

        m.data[12] = -(right + left) / rl;
        m.data[13] = -(top + bottom) / tb;
        m.data[14] = -near_clip / fn;
        m.data[15] = 1.0f;
        return m;
    }

    Mat4 mat4_translation(Vec3 position) {
        Mat4 m = mat4_identity();
        m.data[12] = position.x;
        m.data[13] = position.y;
        m.data[14] = position.z;
        return m;
    }

    Mat4 mat4_scale(Vec3 scale) {
        Mat4 m = mat4_identity();
        m.data[0]  = scale.x;
        m.data[5]  = scale.y;
        m.data[10] = scale.z;
        return m;
    }

    Mat4 mat4_rotation_z(f32 radians) {
        Mat4 m = mat4_identity();
        f32 c = cosf(radians);
        f32 s = sinf(radians);
        // Column-major rotation about Z.
        m.data[0] =  c;
        m.data[1] =  s;
        m.data[4] = -s;
        m.data[5] =  c;
        return m;
    }
}
