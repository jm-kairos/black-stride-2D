#pragma once

#include "defines.h"
#include "math/math_utils.h"

namespace bs_math
{
    struct GridCell {
        i64 x;
        i64 y;
    };

    struct HierPos2 {
        GridCell cell;
        Vec2     local;
    };

    // Default cell size for the game layer (power of two).
    constexpr f32 BS_HIERPOS_CELL_SIZE = 16384.0f;
    constexpr f32 BS_HIERPOS_HALF_CELL = BS_HIERPOS_CELL_SIZE * 0.5f;

    // Convert legacy world-space Vec2 into a canonical HierPos2.
    bs__api__ HierPos2 hierpos_from_vec2(Vec2 world, f32 cell_size);

    // Convert HierPos2 back to legacy Vec2. Safe only when the result is near the origin
    // (i.e. cell is small enough that cell*cell_size fits in f32 without catastrophic loss).
    bs__api__ Vec2 hierpos_to_vec2(const HierPos2* hp, f32 cell_size);

    // High-precision conversion: exact world-space coordinates as f64.
    bs__api__ void hierpos_to_f64(const HierPos2* hp, f32 cell_size, f64* out_x, f64* out_y);

    // Re-canonicalize: fold local into [-cell_size/2, +cell_size/2) by adjusting cell.
    bs__api__ HierPos2 hierpos_normalize(HierPos2 hp, f32 cell_size);

    // Precision-safe lerp between two hierarchical positions.
    // t in [0,1]; result is canonical.
    bs__api__ HierPos2 hierpos_lerp(const HierPos2* a, const HierPos2* b, f32 t, f32 cell_size);

    // Add a high-precision delta (in world units) to a hierarchical position.
    bs__api__ HierPos2 hierpos_add_f64(const HierPos2* hp, f64 dx, f64 dy, f32 cell_size);

    // Convenience: add a small f32 Vec2 delta to a hierarchical position (re-canonicalizes).
    // Uses the default game cell size. For velocity integration and local offsets.
    inline HierPos2 hierpos_add_vec2(const HierPos2* hp, Vec2 d) {
        return hierpos_add_f64(hp, (f64)d.x, (f64)d.y, BS_HIERPOS_CELL_SIZE);
    }

    // High-precision subtraction: compute (a - b) as a small Vec2.
    // Safe for rendering when a and b are near each other (floating origin).
    bs__api__ Vec2 hierpos_diff(const HierPos2* a, const HierPos2* b, f32 cell_size);

    // Convenience wrappers that bind the default game cell size (BS_HIERPOS_CELL_SIZE).
    inline HierPos2 hierpos_from_vec2(Vec2 world) { return hierpos_from_vec2(world, BS_HIERPOS_CELL_SIZE); }
    inline Vec2     hierpos_to_vec2(const HierPos2* hp) { return hierpos_to_vec2(hp, BS_HIERPOS_CELL_SIZE); }
    inline Vec2     hierpos_diff(const HierPos2* a, const HierPos2* b) { return hierpos_diff(a, b, BS_HIERPOS_CELL_SIZE); }

    // Self-test: round-trip a suite of coordinates and assert exact recovery.
    // Returns FALSE if any invariant is violated.
    bs__api__ b8 bs_hierpos_selftest(void);
}
