#pragma once

#include "defines.h"
#include "renderer/renderer_types.h"

// =====================================================================================
// Camera2D helpers (frontend, backend-agnostic — engine math only, no SDL/GPU).
//
// A Camera2D is plain POD the game owns and mutates (pan/zoom/rotate). The renderer calls
// camera2d_view_proj() each frame with the live framebuffer size to produce the matrix the
// sprite shader consumes. Kept as free functions so the game can also project/unproject for
// picking and HUD placement without touching the backend.
// =====================================================================================

// Build a sensible default camera: centered at the world origin, zoom 1.0 (1 world unit = 1
// pixel), no rotation.
bs__api__ Camera2D camera2d_default();

// Compose the view-projection matrix for `cam` given the current framebuffer size in pixels.
// Convention: y-up, origin at screen center, Vulkan/SDL clip z in [0,1]. At zoom 1 and no
// rotation, world point == pixel offset from the window center.
bs__api__ bs_math::Mat4 camera2d_view_proj(const Camera2D* cam, u16 fb_width, u16 fb_height);

// Convert a screen-space pixel coordinate (origin top-left, y-down — as reported by the OS /
// input layer) into a world-space point under `cam`. Inverse of the view-proj pipeline; use
// for mouse picking and placing world objects under the cursor.
bs__api__ bs_math::Vec2 camera2d_screen_to_world(
    const Camera2D* cam, u16 fb_width, u16 fb_height, bs_math::Vec2 screen_px);

// Convert a world-space point under `cam` into a screen-space pixel coordinate (origin
// top-left, y-down). Exact inverse of camera2d_screen_to_world; use to anchor screen-space HUD
// text/markers to world objects (project the object, then draw 2D text at the returned pixel).
bs__api__ bs_math::Vec2 camera2d_world_to_screen(
    const Camera2D* cam, u16 fb_width, u16 fb_height, bs_math::Vec2 world_pt);
