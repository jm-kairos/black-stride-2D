#pragma once
#include <defines.h>
#include <math/math_utils.h>
#include <renderer/renderer_types.h>
struct StarSystem;
struct CelestialBody;
struct PlanetProperties;
// Per-planet-type appearance tunables, edited live in the Planet Editor window and persisted to
// disk. `size_mul` multiplies the planet's physical world radius before the camera transform, so
// planets grow with zoom exactly like the star (which uses star_body_scale); `min_px` is the
// on-screen floor when zoomed out (mirrors STAR_MIN_SCREEN_RADIUS) and there is NO max cap.
struct PlanetTypeParams {
    f32 size_mul;        // multiplier on physical radius (the per-type "radius" control)
    f32 min_px;          // minimum on-screen radius in px when zoomed out
    f32 rotation_speed;  // axial rotation rate (radians/sec)
    f32 halo_scale;      // scales the atmosphere/ring quad margin
    f32 cloud_amount;    // cloud coverage passed to the shader (0..1)
    bs_color surface_color; // base surface color fed to the sphere shader
};
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
    f32 star_body_scale;      // 3D sphere radius multiplier on base_r (large = close to surface)
    f32 planet_rotation_speed; // planet axial rotation rate (radians/sec)
    // ---- Per-type planet editor -----------------------------------------------------------
    // One params row per PlanetType (must match PLANET_TYPE_COUNT; static_assert in the .cpp).
    static const i32 PLANET_EDITOR_TYPE_COUNT = 8;
    PlanetTypeParams planet_params[PLANET_EDITOR_TYPE_COUNT];
    b8  show_planet_editor;    // toggled by the "Planet Properties..." button in the editor panel
    i32 planet_editor_sel_type; // planet type currently shown in the editor window
    b8  planet_use_genome_colors; // ON: per-planet genome palette; OFF: flat per-type editor colour (debug)
    // Real-time GPU procedural star-surface tunables (3D mode uses the star_surface shader)
    f32 surf_noise_scale;      // granulation cell frequency
    f32 surf_flow_speed;       // convection animation speed
    f32 surf_granule_contrast; // granulation contrast around mid-grey
    f32 surf_hotspot_gain;     // emissive bright-spot strength
    f32 surf_sunspot_density;  // dark-spot coverage
    f32 surf_limb_darkening;   // limb darkening exponent
    f32 surf_brightness;       // overall surface brightness
    f32 surf_corona_strength;  // outer corona glow intensity
    f32 surf_corona_ratio;     // outer glow radius / body radius (quad margin)
    f32 surf_dark_radius;      // radius (in body radii) of the opaque black surround
    // ---- Procedural textures (owned) ----------------------------------------------------
    bs_texture tex_core;    // quartic falloff — sharp central disk
    bs_texture tex_corona;  // quadratic falloff — soft mid-glow
    bs_texture tex_halo;    // linear falloff — wide outer aura
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
    // Apply the single-source anamorphic streak/flare renderer state (length, angle,
    // intensity, flare) for the given draw `scale` and `time`. Does NOT set the streak
    // source position (the caller owns that via renderer_set_streak_source). Because the
    // streak is a single global post-process, the last caller wins; call this last for the
    // star that should own the streak (e.g. the current system's hero star).
    void apply_streak_state(f32 scale, f32 time) const;
    // Internal render paths.
    void draw_star_classic(StarSystem& ss, bs_math::Vec2 world_pos, bs_math::Vec2 screen_pos,
                           bs_math::Vec2 aux_world_pos, f32 screen_radius, f32 vis,
                           f32 time, u32 layer, u16 fb_w, u16 fb_h, f32 scale) const;
    void draw_star_3d(StarSystem& ss, bs_math::Vec2 world_pos, bs_math::Vec2 screen_pos,
                      f32 base_r, f32 screen_radius, f32 vis, f32 time, u32 layer,
                      u16 fb_w, u16 fb_h, f32 scale, bs_math::Vec2 aux_world_pos) const;
    // Draw one planet as a procedural impostor sphere (star-lit, per-type surface, clouds,
    // atmosphere, rings). `screen_pos` = planet centre in screen px; `body_radius_px` = sphere
    // radius in screen px; `vis` already folds in any cross-fade weight; light direction + surface
    // seed are derived from the planet's orbital offset and elements.
    void draw_planet_3d(const CelestialBody& planet, const PlanetProperties& props,
                        bs_color star_color, bs_math::Vec2 screen_pos, f32 body_radius_px,
                        f32 vis, f32 time, u32 layer, u16 fb_w, u16 fb_h) const;
    // ---- Editor UI ----------------------------------------------------------------------
    void build_ui();
    // Free-floating window to edit per-type planet appearance (size, color, rotation, ...).
    void build_planet_editor();
    // Reset every planet type back to its built-in default appearance.
    void planet_params_reset_defaults();
    // Persist / restore planet_params to bin/planet_editor.cfg. Return TRUE on success.
    b8 planet_params_save() const;
    b8 planet_params_load();
};
// Build a bs_light2d for a star from pre-computed world-space values.
// `star_pos`   = camera-relative position of the star sprite.
// `star_color` = the star's intrinsic color.
// `max_orbit`  = largest semi_major_axis among the system's planets.
// `vis`        = sensor visibility [0,1].
// `intensity_mul` = editor multiplier on star light intensity.
// `radius_mul`    = editor multiplier on star light radius.
static inline bs_light2d make_star_light(bs_math::Vec2 star_pos, bs_color star_color,
                                          f32 max_orbit, f32 vis,
                                          f32 intensity_mul = 1.0f,
                                          f32 radius_mul = 1.0f)
{
    bs_light2d light{};
    light.position  = star_pos;
    light.color     = star_color;
    light.radius    = max_orbit * 4.0f * radius_mul;
    light.intensity = 5.0f * vis * intensity_mul;
    light.enabled   = TRUE;
    return light;
}
