#pragma once

#include "defines.h"

namespace ibx_math
{
    u32 clamp(u32 d, u32 min, u32 max);

    typedef struct Vec2
    {
        f32 x;
        f32 y;
    } Vec2f32;

    typedef struct Vec3
    {
        f32 x;
        f32 y;
        f32 z;
    } Vec3f32;

    typedef struct Vec4
    {
        f32 x;
        f32 y;
        f32 z;
        f32 w;
    } Vec4f32;

}
