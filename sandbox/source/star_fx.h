#pragma once
#include <defines.h>
#include <math/math_utils.h>
#include <renderer/renderer_types.h>
struct StarSystem;
// Multi-layer star visual effects: core, corona, halo sprites + anamorphic streak post-process.
// Owns its procedural textures; draw_star submits sprites to the renderer batch.
struct StarFxSystem {
    // ---- Editor-tunable state -----------------------------------------------------------
    // Anamorphic streak post-process (aux-bloom path)
    b8  streak_enabled;
    f32 streak_angle;       // degrees in UI
    f32 streak_length;
    f32 streak_intensity;
    f32 streak_flare_intensity; // lens-flare ghost intensity
    f32 streak_rotation_speed; // deg/s, 0 = static
    f32 streak_pulse_speed;  // Hz, 0 = static
    f32 streak_length_mul;    // gameplay-derived multiplier (default 1.0)
    f32 streak_intensity_mul; // gameplay-derived multiplier (default 1.0)
    // 3D sphere vs classic glow toggle
    b8  star_3d_mode;
    f32 star_rotation_speed;  // deg/s, 0 = static
    // ---- Procedural textures (owned) ----------------------------------------------------
    bs_texture tex_core;    // quartic falloff — sharp central disk
    bs_texture tex_corona;  // quadratic falloff — soft mid-glow
    bs_texture tex_halo;    // linear falloff — wide outer aura
    bs_texture tex_sphere;  // procedural lit sphere for 3D mode
    // ---- Lifetime -----------------------------------------------------------------------
    void init();
    void shutdown();
    // ---- Rendering ----------------------------------------------------------------------
    // Draws one star at `pos` with visibility `vis`.
    // `base_r` = ss.star.radius * (0.3 + 0.7*vis); `time` = galaxy_map_time.
    // `layer` is typically LAYER_CELESTIAL.
    // `world_pos`      = star centre in world space (for sprite batch).
    // `screen_pos`     = star centre in screen pixels (for sunburst shader).
    // `base_r`         = star radius in world units (for sprite sizing).
    // `screen_radius`  = star radius in screen pixels (for sunburst shader).
    // `fb_w`, `fb_h`   = viewport size in pixels.
    void draw_star(StarSystem& ss, bs_math::Vec2 world_pos, bs_math::Vec2 screen_pos,
                   f32 base_r, f32 screen_radius, f32 vis, f32 time, u32 layer,
                   u16 fb_w, u16 fb_h, f32 scale = 1.0f,
                   bs_math::Vec2 aux_world_pos = bs_math::Vec2{0.0f, 0.0f}) const;
    // Internal render paths.
    void draw_star_classic(StarSystem& ss, bs_math::Vec2 world_pos, bs_math::Vec2 screen_pos,
                           bs_math::Vec2 aux_world_pos, f32 screen_radius, f32 vis,
                           f32 time, u32 layer, u16 fb_w, u16 fb_h, f32 scale) const;
    void draw_star_3d(StarSystem& ss, bs_math::Vec2 world_pos, f32 base_r, f32 vis,
                      f32 time, u32 layer, f32 scale) const;
    // ---- Editor UI ----------------------------------------------------------------------
    void build_ui();
};
// Build a bs_light2d for a star from pre-computed world-space values.
// `star_pos`   = camera-relative compressed position of the star sprite.
// `star_color` = the star's intrinsic color.
// `max_orbit`  = largest semi_major_axis among the system's planets.
// `comp`       = compression_factor(zoom) used to keep radius zoom-independent.
// `vis`        = sensor visibility [0,1].
// `intensity_mul` = editor multiplier on star light intensity.
// `radius_mul`    = editor multiplier on star light radius.
static inline bs_light2d make_star_light(bs_math::Vec2 star_pos, bs_color star_color,
                                          f32 max_orbit, f32 comp, f32 vis,
                                          f32 intensity_mul = 1.0f,
                                          f32 radius_mul = 1.0f)
{
    bs_light2d light{};
    light.position  = star_pos;
    light.color     = star_color;
    light.radius    = max_orbit * 4.0f * comp * radius_mul;
    light.intensity = 5.0f * vis * intensity_mul;
    light.enabled   = TRUE;
    return light;
}
