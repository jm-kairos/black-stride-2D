#include "renderer/camera2d.h"

#include "math/math_utils.h"

// =====================================================================================
// Camera2D implementation. Pure math; no backend. The view matrix is the inverse of the
// camera's world placement (translate to position, rotate, scale by zoom), and the projection
// is a centered y-up ortho sized to the framebuffer.
// =====================================================================================

using namespace bs_math;

Camera2D camera2d_default()
{
    Camera2D cam;
    cam.position = Vec2{ 0.0f, 0.0f };
    cam.zoom     = 1.0f;
    cam.rotation = 0.0f;
    return cam;
}

Mat4 camera2d_view_proj(const Camera2D* cam, u16 fb_width, u16 fb_height)
{
    f32 hw = (f32)fb_width  * 0.5f;
    f32 hh = (f32)fb_height * 0.5f;

    // Centered, y-up orthographic projection, clip z in [0,1] (Vulkan/SDL convention).
    Mat4 proj = mat4_ortho(-hw, hw, -hh, hh, -1.0f, 1.0f);

    // View = inverse of the camera's world transform. We build it directly as
    //   scale(zoom) * rotateZ(-rotation) * translate(-position)
    // so a world point maps into centered pixel space, scaled by zoom.
    f32 z = (cam->zoom != 0.0f) ? cam->zoom : 1.0f;

    Mat4 t = mat4_translation(Vec3{ -cam->position.x, -cam->position.y, 0.0f });
    Mat4 r = mat4_rotation_z(-cam->rotation);
    Mat4 s = mat4_scale(Vec3{ z, z, 1.0f });

    Mat4 view = mat4_mul(s, mat4_mul(r, t));
    return mat4_mul(proj, view);
}

Vec2 camera2d_screen_to_world(
    const Camera2D* cam, u16 fb_width, u16 fb_height, Vec2 screen_px)
{
    f32 hw = (f32)fb_width  * 0.5f;
    f32 hh = (f32)fb_height * 0.5f;

    // Screen px (origin top-left, y-down) -> centered, y-up pixel offset. These centered
    // coordinates ARE the view-space coordinates (the ortho maps them linearly to clip).
    Vec2 view_pt = Vec2{ screen_px.x - hw, hh - screen_px.y };

    // Invert the view: world = position + rotateZ(+rotation) * (view_pt / zoom).
    f32 z = (cam->zoom != 0.0f) ? cam->zoom : 1.0f;
    Vec2 unscaled = vec2_scale(view_pt, 1.0f / z);
    Vec2 unrotated = vec2_rotate(unscaled, cam->rotation);
    return vec2_add(cam->position, unrotated);
}

Vec2 camera2d_world_to_screen(
    const Camera2D* cam, u16 fb_width, u16 fb_height, Vec2 world_pt)
{
    f32 hw = (f32)fb_width  * 0.5f;
    f32 hh = (f32)fb_height * 0.5f;

    // Forward view transform (exact inverse of camera2d_screen_to_world):
    //   view_pt = zoom * rotateZ(-rotation) * (world - position)
    f32 z = (cam->zoom != 0.0f) ? cam->zoom : 1.0f;
    Vec2 rel = vec2_sub(world_pt, cam->position);
    Vec2 rotated = vec2_rotate(rel, -cam->rotation);
    Vec2 view_pt = vec2_scale(rotated, z);

    // Centered, y-up view space -> screen px (origin top-left, y-down).
    return Vec2{ view_pt.x + hw, hh - view_pt.y };
}
