#pragma once

#include "defines.h"

namespace bs_math
{
    constexpr f32 BS_PI = 3.14159265358979323846f;
    constexpr f32 BS_DEG2RAD = BS_PI / 180.0f;
    constexpr f32 BS_RAD2DEG = 180.0f / BS_PI;

    bs__api__ u32 clamp(u32 d, u32 min, u32 max);
    bs__api__ f32 clampf(f32 d, f32 min, f32 max);

    typedef struct bs__api__ Vec2 
    {
        f32 x;
        f32 y;
    } Vec2f32;

    typedef struct bs__api__ Vec3
    {
        f32 x;
        f32 y;
        f32 z;
    } Vec3f32;

    typedef struct bs__api__ Vec4
    {
        f32 x;
        f32 y;
        f32 z;
        f32 w;
    } Vec4f32;

    // 4x4 matrix, column-major storage: data[col * 4 + row].
    // This matches the layout SPIR-V / Vulkan (and SDL GPU) expect for a
    // column-major mat4 uniform, so it can be uploaded directly.
    typedef struct bs__api__ Mat4
    {
        f32 data[16];
    } Mat4;

    // ---- Vec2 ops ----
    bs__api__ Vec2 vec2_add(Vec2 a, Vec2 b);
    bs__api__ Vec2 vec2_sub(Vec2 a, Vec2 b);
    bs__api__ Vec2 vec2_scale(Vec2 a, f32 s);
    bs__api__ f32  vec2_dot(Vec2 a, Vec2 b);
    bs__api__ f32  vec2_length(Vec2 a);
    bs__api__ Vec2 vec2_normalized(Vec2 a);
    // Rotate a 2D vector by `radians` (counter-clockwise).
    bs__api__ Vec2 vec2_rotate(Vec2 a, f32 radians);

    // ---- Vec3 ops ----
    bs__api__ Vec3 vec3_add(Vec3 a, Vec3 b);
    bs__api__ Vec3 vec3_sub(Vec3 a, Vec3 b);
    bs__api__ Vec3 vec3_scale(Vec3 a, f32 s);

    // ---- Mat4 ops ----
    bs__api__ Mat4 mat4_identity();
    // out = a * b (column-major; transforming a column vector v as out*v applies
    // b first, then a).
    bs__api__ Mat4 mat4_mul(Mat4 a, Mat4 b);
    // Orthographic projection mapping [left,right] x [bottom,top] x [near,far]
    // into clip space using the Vulkan/SDL-GPU convention (z in [0,1]).
    bs__api__ Mat4 mat4_ortho(f32 left, f32 right, f32 bottom, f32 top, f32 near_clip, f32 far_clip);
    bs__api__ Mat4 mat4_translation(Vec3 position);
    bs__api__ Mat4 mat4_scale(Vec3 scale);
    // Rotation about the Z axis (the only rotation a 2D game needs).
    bs__api__ Mat4 mat4_rotation_z(f32 radians);
}
