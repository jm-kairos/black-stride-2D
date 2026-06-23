#include "star_fx.h"
#include "game.h"
#include <core/logger.h>
#include <renderer/renderer.h>
#include <renderer/bs_ui.h>
#include <cstdio>
// ---- Texture generation ---------------------------------------------------------------
static bs_texture make_radial_texture(u32 size, f32 (*falloff_fn)(f32 t))
{
    const u32 half = size / 2;
    static u8 pixels[256 * 256 * 4]; // max size, static to avoid stack overflow
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            f32 dx = (f32)x - half + 0.5f;
            f32 dy = (f32)y - half + 0.5f;
            f32 dist = sqrtf(dx * dx + dy * dy) / (f32)half;
            f32 t = dist > 1.0f ? 1.0f : dist;
            f32 alpha = falloff_fn(t);
            u32 i = (y * size + x) * 4;
            pixels[i + 0] = 255;
            pixels[i + 1] = 255;
            pixels[i + 2] = 255;
            pixels[i + 3] = (u8)(alpha * 255.0f);
        }
    }
    return renderer_create_texture(pixels, size, size);
}
static f32 falloff_core(f32 t)   { return 1.0f - t * t * t * t; }
static f32 falloff_corona(f32 t) { return 1.0f - t * t; }
static f32 falloff_halo(f32 t)   { return 1.0f - t; }
// ---- Sphere texture: ray-march a lit sphere into an RGBA buffer ------------------------
static bs_texture make_sphere_texture(u32 size)
{
    const u32 half = size / 2;
    static u8 pixels[256 * 256 * 4];
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            f32 dx = ((f32)x - half + 0.5f) / (f32)half;
            f32 dy = ((f32)y - half + 0.5f) / (f32)half;
            f32 r2 = dx * dx + dy * dy;
            u32 i = (y * size + x) * 4;
            if (r2 > 1.0f) {
                pixels[i + 0] = 0;
                pixels[i + 1] = 0;
                pixels[i + 2] = 0;
                pixels[i + 3] = 0;
                continue;
            }
            // --- Limb darkening: stars are brighter in center, dimmer at edge ---
            f32 limb = sqrtf(1.0f - r2);
            f32 limb_dark = powf(limb, 0.7f); // <1 makes edges darker
            // --- Solar granulation: layered sine waves for organic surface detail ---
            f32 n1 = sinf(dx *  8.3f + dy *  5.7f);
            f32 n2 = sinf(dx * 15.1f - dy * 12.3f);
            f32 n3 = sinf(dx * 23.7f + dy * 19.4f);
            f32 noise = (n1 * 0.50f + n2 * 0.30f + n3 * 0.20f) * 0.5f + 0.5f;
            // --- Brightness: base luminosity * limb darkening * surface texture ---
            f32 bright = limb_dark * (0.55f + 0.45f * noise);
            if (bright > 1.0f) bright = 1.0f;
            // --- Color temperature: center is hotter (bluer-white), edge cooler (yellow-white) ---
            f32 t = 1.0f - limb_dark; // 0 at center, 1 at edge
            f32 r = 1.0f;
            f32 g = 1.0f - t * 0.15f;
            f32 b = 1.0f - t * 0.30f;
            // --- Alpha: full disk, soft edge fade at perimeter ---
            f32 alpha = 1.0f - (r2 - 0.82f) / 0.18f;
            if (alpha > 1.0f) alpha = 1.0f;
            if (alpha < 0.0f) alpha = 0.0f;
            // Premultiply RGB by brightness so shadow pixels are genuinely dark
            pixels[i + 0] = (u8)(r * bright * 255.0f);
            pixels[i + 1] = (u8)(g * bright * 255.0f);
            pixels[i + 2] = (u8)(b * bright * 255.0f);
            pixels[i + 3] = (u8)(alpha * 255.0f);
        }
    }
    return renderer_create_texture(pixels, size, size);
}
// ---- Lifetime -------------------------------------------------------------------------
void StarFxSystem::init()
{
    streak_enabled   = FALSE;
    streak_angle     = 0.0f;
    streak_length    = 3.0f;
    streak_intensity = 0.5f;
    streak_flare_intensity = 1.0f;
    streak_rotation_speed = 0.0f;
    streak_pulse_speed  = 0.0f;
    streak_length_mul    = 1.0f;
    streak_intensity_mul = 1.0f;
    star_3d_mode       = FALSE;
    star_rotation_speed = 5.0f;
    const u32 STAR_TEX_SIZE = 256;
    tex_core   = make_radial_texture(STAR_TEX_SIZE, falloff_core);
    tex_corona = make_radial_texture(STAR_TEX_SIZE, falloff_corona);
    tex_halo   = make_radial_texture(STAR_TEX_SIZE, falloff_halo);
    tex_sphere = make_sphere_texture(STAR_TEX_SIZE);
}
void StarFxSystem::shutdown()
{
    renderer_destroy_texture(tex_core);
    renderer_destroy_texture(tex_corona);
    renderer_destroy_texture(tex_halo);
    renderer_destroy_texture(tex_sphere);
}
// ---- Rendering ------------------------------------------------------------------------
void StarFxSystem::draw_star(StarSystem& ss, bs_math::Vec2 world_pos, bs_math::Vec2 screen_pos,
                             f32 base_r, f32 screen_radius, f32 vis, f32 time, u32 layer,
                             u16 fb_w, u16 fb_h, f32 scale, bs_math::Vec2 aux_world_pos) const
{
    if (star_3d_mode)
        draw_star_3d(ss, world_pos, base_r, vis, time, layer, scale);
    else
        draw_star_classic(ss, world_pos, screen_pos, aux_world_pos, screen_radius, vis,
                          time, layer, fb_w, fb_h, scale);
}
void StarFxSystem::draw_star_classic(StarSystem& ss, bs_math::Vec2 world_pos, bs_math::Vec2 screen_pos,
                                     bs_math::Vec2 aux_world_pos, f32 screen_radius, f32 vis,
                                     f32 time, u32 layer, u16 fb_w, u16 fb_h, f32 scale) const
{
    renderer_set_streak_enabled(streak_enabled);
    if (streak_enabled)
    {
        f32 scaled_streak_length = streak_length * scale * streak_length_mul;
        if (scaled_streak_length > 50.0f) scaled_streak_length = 50.0f;
        f32 effective_angle = streak_angle + streak_rotation_speed * time;
        renderer_set_streak_params(effective_angle * bs_math::BS_DEG2RAD, scaled_streak_length);
        f32 effective_intensity = streak_intensity * streak_intensity_mul;
        if (streak_pulse_speed > 0.001f)
        {
            f32 pulse = 0.7f + 0.3f * sinf(time * streak_pulse_speed * 6.28318530718f);
            effective_intensity *= pulse;
        }
        renderer_set_streak_intensity(effective_intensity);
        renderer_set_streak_flare_intensity(streak_flare_intensity * streak_intensity_mul);
    }
    // ---- Procedural sunburst shader (replaces 3-sprite classic stack) -------------------
    bs_sunburst_params params{};
    params.screen_pos        = screen_pos;
    params.world_pos         = world_pos;
    params.aux_bloom_world_pos = (aux_world_pos.x == 0.0f && aux_world_pos.y == 0.0f)
                                 ? world_pos : aux_world_pos;
    f32 glow_r = screen_radius * 3.0f;                // warm corona
    if (glow_r > 250.0f) glow_r = 250.0f;            // cap: prevent GPU timeout from huge quad
    f32 body_r = screen_radius * 0.30f;             // tiny hot core
    if (body_r > glow_r * 0.5f) body_r = glow_r * 0.5f; // cap body to half of glow
    params.body_radius = body_r;
    params.glow_radius = glow_r;
    params.color       = ss.star.color;
    params.time        = time;
    params.visibility  = vis;
    params.layer       = layer;
    params.fb_w        = fb_w;
    params.fb_h        = fb_h;
    params.aux_bloom   = streak_enabled; // allow classic sunburst to generate streaks
    // ---- Safety guards: reject draw if parameters are pathological -----------------------
    b8 params_ok = TRUE;
    auto is_nan_inf = [](f32 v) { return v != v || v > 1e9f || v < -1e9f; };
    if (is_nan_inf(glow_r) || glow_r < 1.0f || glow_r > 500.0f)
    { BS_LOG_WARN("SUNBURST: glow_r bad: %.2f", glow_r); params_ok = FALSE; }
    if (is_nan_inf(body_r) || body_r < 0.5f || body_r > 300.0f)
    { BS_LOG_WARN("SUNBURST: body_r bad: %.2f", body_r); params_ok = FALSE; }
    if (is_nan_inf(screen_pos.x) || is_nan_inf(screen_pos.y) ||
        screen_pos.x < -2000.0f || screen_pos.x > 10000.0f ||
        screen_pos.y < -2000.0f || screen_pos.y > 10000.0f)
    { BS_LOG_WARN("SUNBURST: screen_pos bad: (%.2f, %.2f)", screen_pos.x, screen_pos.y); params_ok = FALSE; }
    if (fb_w == 0 || fb_h == 0) { BS_LOG_WARN("SUNBURST: fb size zero"); params_ok = FALSE; }
    if (is_nan_inf(vis) || vis < 0.0f || vis > 1.0f) { BS_LOG_WARN("SUNBURST: vis bad: %.3f", vis); params_ok = FALSE; }
    if (params_ok)
        renderer_draw_sunburst(&params);
}
void StarFxSystem::draw_star_3d(StarSystem& ss, bs_math::Vec2 pos, f32 base_r, f32 vis,
                                f32 time, u32 layer, f32 scale) const
{
    renderer_set_streak_enabled(streak_enabled);
    if (streak_enabled)
    {
        f32 scaled_streak_length = streak_length * scale * streak_length_mul;
        if (scaled_streak_length > 50.0f) scaled_streak_length = 50.0f;
        f32 effective_angle = streak_angle + streak_rotation_speed * time;
        renderer_set_streak_params(effective_angle * bs_math::BS_DEG2RAD, scaled_streak_length);
        f32 effective_intensity = streak_intensity * streak_intensity_mul;
        if (streak_pulse_speed > 0.001f)
        {
            f32 pulse = 0.7f + 0.3f * sinf(time * streak_pulse_speed * 6.28318530718f);
            effective_intensity *= pulse;
        }
        renderer_set_streak_intensity(effective_intensity);
        renderer_set_streak_flare_intensity(streak_flare_intensity * streak_intensity_mul);
    }
    // ---- Layer 0: Lit Sphere Body -------------------------------------------------------
    // Texture has limb darkening + surface noise baked into RGB. Dark spots are dim
    // (low RGB), not transparent — they add little light in additive blending.
    {
        f32 rotation = star_rotation_speed * time * bs_math::BS_DEG2RAD;
        f32 body_r = base_r * 0.80f;
        bs_color body_tint = ss.star.color;
        body_tint.a = vis;
        bs_sprite sp{};
        sp.position      = pos;
        sp.size          = bs_math::Vec2{ body_r * 2.0f, body_r * 2.0f };
        sp.origin        = bs_math::Vec2{ 0.5f, 0.5f };
        sp.rotation      = rotation;
        sp.uv            = bs_rect{ 0.0f, 0.0f, 1.0f, 1.0f };
        sp.tint          = body_tint;
        sp.texture       = tex_sphere;
        sp.blend         = BLEND_ADDITIVE;
        sp.layer         = layer;
        sp.glow_override = nullptr;
        renderer_draw_sprite(&sp);
    }
    // ---- Layer 1: Subtle Corona Atmosphere ----------------------------------------------
    {
        f32 corona_pulse = 1.0f + 0.05f * sinf(time * 1.2f + ss.corona_pulse_phase);
        f32 corona_r = base_r * 0.55f * corona_pulse;
        bs_glow_params& g1 = ss.glow[1];
        g1.intensity    = 1.5f;  g1.falloff      = 1.5f;
        g1.head_mult    = 0.0f;  g1.head_falloff = 1.0f;
        g1.head_range   = 0.0f;  g1.distort_amp  = 0.0f;
        g1.wave_speed   = 0.0f;  g1.wave_freq    = 0.0f;
        g1.jitter_speed = 0.0f;  g1.jitter_freq  = 0.0f;
        g1.glow_tint    = ss.star.color;
        bs_color corona_tint = ss.star.color;
        corona_tint.a = vis * 0.6f;
        bs_sprite sp{};
        sp.position      = pos;
        sp.size          = bs_math::Vec2{ corona_r * 2.0f, corona_r * 2.0f };
        sp.origin        = bs_math::Vec2{ 0.5f, 0.5f };
        sp.rotation      = 0.0f;
        sp.uv            = bs_rect{ 0.0f, 0.0f, 1.0f, 1.0f };
        sp.tint          = corona_tint;
        sp.custom        = bs_color{ corona_pulse, 0.0f, 0.0f, 0.0f };
        sp.texture       = tex_corona;
        sp.blend         = BLEND_ADDITIVE;
        sp.layer         = layer;
        sp.glow_override = &g1;
        renderer_draw_sprite(&sp);
    }
    // ---- Layer 2: Very Faint Outer Halo -------------------------------------------------
    {
        f32 halo_pulse = 1.0f + 0.08f * sinf(time * 0.7f + ss.halo_pulse_phase);
        f32 halo_r = base_r * 1.10f * halo_pulse;
        bs_glow_params& g2 = ss.glow[2];
        g2.intensity    = 0.6f;  g2.falloff      = 0.8f;
        g2.head_mult    = 0.0f;  g2.head_falloff = 1.0f;
        g2.head_range   = 0.0f;  g2.distort_amp  = 0.0f;
        g2.wave_speed   = 0.0f;  g2.wave_freq    = 0.0f;
        g2.jitter_speed = 0.0f;  g2.jitter_freq  = 0.0f;
        g2.glow_tint    = ss.star.color;
        bs_color halo_tint = ss.star.color;
        halo_tint.a = vis * 0.20f;
        bs_sprite sp{};
        sp.position      = pos;
        sp.size          = bs_math::Vec2{ halo_r * 2.0f, halo_r * 2.0f };
        sp.origin        = bs_math::Vec2{ 0.5f, 0.5f };
        sp.rotation      = 0.0f;
        sp.uv            = bs_rect{ 0.0f, 0.0f, 1.0f, 1.0f };
        sp.tint          = halo_tint;
        sp.custom        = bs_color{ halo_pulse, 0.0f, 0.0f, 0.0f };
        sp.texture       = tex_halo;
        sp.blend         = BLEND_ADDITIVE;
        sp.layer         = layer;
        sp.glow_override = &g2;
        renderer_draw_sprite(&sp);
    }
}
// ---- Editor UI ------------------------------------------------------------------------
void StarFxSystem::build_ui()
{
    const f32 SP[4] = { 0.95f, 0.75f, 0.35f, 1.0f };
    bs_ui_text_colored(SP[0], SP[1], SP[2], SP[3], "STAR FX");
    bs_ui_text_colored(SP[0], SP[1], SP[2], SP[3], "Anamorphic Streaks");
    bool streak_on = streak_enabled ? true : false;
    bs_ui_checkbox("Streaks enabled", &streak_on);
    streak_enabled = streak_on ? TRUE : FALSE;
    if (streak_enabled)
    {
        bs_ui_slider_float("Streak angle",     &streak_angle,     0.0f, 180.0f);
        bs_ui_slider_float("Streak length",    &streak_length,    1.0f, 50.0f);
        bs_ui_slider_float("Streak intensity", &streak_intensity, 0.0f, 2.0f);
        bs_ui_slider_float("Flare intensity",  &streak_flare_intensity, 0.0f, 1.0f);
        bs_ui_slider_float("Rotation speed",   &streak_rotation_speed, 0.0f, 90.0f);
        bs_ui_slider_float("Pulse speed",      &streak_pulse_speed,    0.0f, 5.0f);
    }
    bs_ui_text_colored(SP[0], SP[1], SP[2], SP[3], "Star Rendering");
    bool mode_3d = star_3d_mode ? true : false;
    bs_ui_checkbox("3D sphere mode", &mode_3d);
    star_3d_mode = mode_3d ? TRUE : FALSE;
    if (star_3d_mode)
    {
        bs_ui_slider_float("Rotation speed", &star_rotation_speed, 0.0f, 30.0f);
    }
}
