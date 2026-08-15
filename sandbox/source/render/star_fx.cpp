// Silence MSVC CRT "use fopen_s/sscanf_s" deprecation warnings for the config file IO below
// (sandbox build doesn't define _CRT_SECURE_NO_WARNINGS the way the engine build does).
#define _CRT_SECURE_NO_WARNINGS
#include "render/star_fx.h"
#include "game.h"
#include <core/logger.h>
#include <renderer/renderer.h>
#include <renderer/bs_ui.h>
#include <cstdio>
#include <cstring> // memcmp (save-on-change in build_ui)
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
    star_3d_mode       = TRUE;  // 3D procedural sphere is the default star geometry
    star_rotation_speed = 5.0f;
    star_body_scale    = 8.0f;
    planet_rotation_speed = 0.15f;
    // God rays + mapped-hull backlight (overridden from disk below if a "g" line exists).
    godray_intensity     = 0.55f;
    godray_density       = 0.9f;
    godray_decay         = 0.965f;
    godray_exposure      = 1.25f;
    godray_halo          = 2.5f;
    godray_transmission  = 0.35f;
    backlit_transmission = 0.55f;
    backlit_rim          = 0.85f;
    // Per-type planet appearance: start from built-in defaults, then override from disk if present.
    show_planet_editor    = FALSE;
    planet_editor_sel_type = 0;
    planet_use_genome_colors = TRUE;
    planet_params_reset_defaults();
    planet_params_load();
    // GPU procedural star-surface defaults (sun-like photosphere)
    surf_noise_scale      = 7.0f;
    surf_flow_speed       = 0.15f;
    surf_granule_contrast = 1.4f;
    surf_hotspot_gain     = 0.6f;
    surf_sunspot_density  = 0.4f;
    surf_limb_darkening   = 0.9f;
    surf_brightness       = 1.0f;
    surf_corona_strength  = 0.5f;
    surf_corona_ratio     = 1.9f;
    surf_dark_radius      = 5.0f;
    const u32 STAR_TEX_SIZE = 256;
    tex_core   = make_radial_texture(STAR_TEX_SIZE, falloff_core);
    tex_corona = make_radial_texture(STAR_TEX_SIZE, falloff_corona);
    tex_halo   = make_radial_texture(STAR_TEX_SIZE, falloff_halo);
}
void StarFxSystem::shutdown()
{
    planet_params_save();
    renderer_destroy_texture(tex_core);
    renderer_destroy_texture(tex_corona);
    renderer_destroy_texture(tex_halo);
}
// ---- Rendering ------------------------------------------------------------------------
void StarFxSystem::apply_streak_state(f32 scale, f32 time) const
{
    renderer_set_streak_enabled(streak_enabled);
    if (!streak_enabled) return;
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
void StarFxSystem::draw_star(StarSystem& ss, bs_math::Vec2 world_pos, bs_math::Vec2 screen_pos,
                             f32 base_r, f32 screen_radius, f32 vis, f32 time, u32 layer,
                             u16 fb_w, u16 fb_h, f32 scale, bs_math::Vec2 aux_world_pos) const
{
    if (star_3d_mode)
        draw_star_3d(ss, world_pos, screen_pos, base_r, screen_radius, vis, time, layer,
                     fb_w, fb_h, scale, aux_world_pos);
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
        apply_streak_state(scale, time);
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
    // ---- Safety guards: silently reject draw if parameters are pathological ---------------
    // These are normal cull cases (e.g. an off-screen star), not errors, so they must not log:
    // logging here spams the console every frame while panning/zooming the galaxy map.
    b8 params_ok = TRUE;
    auto is_nan_inf = [](f32 v) { return v != v || v > 1e9f || v < -1e9f; };
    if (is_nan_inf(glow_r) || glow_r < 1.0f || glow_r > 500.0f)
        params_ok = FALSE;
    if (is_nan_inf(body_r) || body_r < 0.5f || body_r > 300.0f)
        params_ok = FALSE;
    if (is_nan_inf(screen_pos.x) || is_nan_inf(screen_pos.y) ||
        screen_pos.x < -2000.0f || screen_pos.x > 10000.0f ||
        screen_pos.y < -2000.0f || screen_pos.y > 10000.0f)
        params_ok = FALSE;
    if (fb_w == 0 || fb_h == 0) params_ok = FALSE;
    if (is_nan_inf(vis) || vis < 0.0f || vis > 1.0f) params_ok = FALSE;
    if (params_ok)
        renderer_draw_sunburst(&params);
}
void StarFxSystem::draw_star_3d(StarSystem& ss, bs_math::Vec2 pos, bs_math::Vec2 screen_pos,
                                f32 base_r, f32 screen_radius, f32 vis, f32 time, u32 layer,
                                u16 fb_w, u16 fb_h, f32 scale, bs_math::Vec2 aux_world_pos) const
{
    (void)base_r;
    // Anamorphic streaks are driven the same way as the classic path.
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

    // ---- Real-time GPU procedural photosphere -------------------------------------------
    // A dedicated screen-space quad shader draws the star's surface (granulation, hotspots,
    // sunspots, limb darkening) plus a corona glow, all animated on the GPU. It renders
    // before the sprite batch so ships/planets occlude it correctly.
    f32 body_r_screen = screen_radius * star_body_scale;
    // Quad must contain the full dark pocket plus its outer fade band (frag fades from
    // dark_radius to dark_radius*1.35). Size it a bit beyond that so the fade completes
    // inside the quad in every direction (the guard-band clamp in the vertex shader keeps
    // this from overflowing the rasterizer when zoomed in close).
    f32 glow_r_screen = body_r_screen * (surf_dark_radius + 0.5f);

    bs_starsurface_params params{};
    params.screen_pos         = screen_pos;
    params.world_pos          = pos;
    params.aux_bloom_world_pos = aux_world_pos;
    params.body_radius        = body_r_screen;
    params.glow_radius        = glow_r_screen;
    params.color              = ss.star.color;
    params.time               = time;
    params.visibility         = vis;
    params.noise_scale        = surf_noise_scale;
    params.flow_speed         = surf_flow_speed;
    params.granule_contrast   = surf_granule_contrast;
    params.hotspot_gain       = surf_hotspot_gain;
    params.sunspot_density    = surf_sunspot_density;
    params.limb_darkening     = surf_limb_darkening;
    params.brightness         = surf_brightness;
    params.corona_strength    = surf_corona_strength;
    params.dark_radius        = surf_dark_radius;
    params.layer              = layer;
    params.fb_w               = fb_w;
    params.fb_h               = fb_h;
    params.aux_bloom          = streak_enabled ? TRUE : FALSE;

    renderer_draw_starsurface(&params);
}

void StarFxSystem::draw_planet_3d(const CelestialBody& planet, const PlanetProperties& props,
                                  bs_color star_color, bs_math::Vec2 screen_pos, f32 body_radius_px,
                                  f32 vis, f32 time, u32 layer, u16 fb_w, u16 fb_h) const
{
    if (body_radius_px < 1.0f || vis <= 0.0f) return;
    const PlanetTypeParams& pe = planet_params[(i32)props.type];
    bs_planetsurface_params pp{};
    pp.screen_pos  = screen_pos;
    pp.body_radius = body_radius_px;
    pp.quad_radius = body_radius_px * pe.halo_scale * (props.has_rings ? 2.4f : (props.has_atmosphere ? 1.6f : 1.2f));
    pp.base_color  = pe.surface_color;
    pp.time        = time;
    // Light points from the planet toward its star (star sits at the system centre, so the
    // direction is -planet.position). World y is up but screen y is down -> flip y.
    f32 lx = -planet.position.x;
    f32 ly =  planet.position.y;
    f32 ll = sqrtf(lx * lx + ly * ly);
    pp.light_dir   = (ll > 1e-3f) ? bs_math::Vec2{ lx / ll, ly / ll } : bs_math::Vec2{ 1.0f, 0.0f };
    pp.planet_type = (i32)props.type;
    pp.visibility  = vis;
    f32 seed       = props.orbit_au * 3.7f + planet.arg_periapsis;
    pp.seed        = seed;
    pp.rotation    = pe.rotation_speed * time + seed;
    pp.has_atmosphere = props.has_atmosphere;
    pp.has_rings   = props.has_rings;
    // ---- Per-planet genome -> data-driven surface (4-stop palette + feature genes) ----
    const PlanetGenome& gn = props.genome;
    b8 genome_ok = (gn.pal_mid.r + gn.pal_mid.g + gn.pal_mid.b) > 0.001f && gn.noise_freq > 0.05f;
    if (planet_use_genome_colors && genome_ok) {
        pp.pal_deep   = gn.pal_deep;
        pp.pal_mid    = gn.pal_mid;
        pp.pal_light  = gn.pal_light;
        pp.pal_accent = gn.pal_accent;
        pp.cloud_tint = gn.cloud_tint;
        pp.atmo_tint  = gn.atmo_tint;
        pp.cloud_amount = gn.cloud_cover;
    } else {
        // Debug / fallback: flat per-type editor colour with no palette variation.
        pp.pal_deep = pp.pal_mid = pp.pal_light = pp.pal_accent = pe.surface_color;
        pp.cloud_tint = bs_color{ 1.0f, 1.0f, 1.0f, 1.0f };
        pp.atmo_tint  = bs_color{ 0.70f, 0.70f, 0.80f, 1.0f };
        pp.cloud_amount = pe.cloud_amount;
    }
    pp.noise_freq      = genome_ok ? gn.noise_freq      : 1.0f;
    pp.warp_amount     = genome_ok ? gn.warp_amount     : 0.7f;
    pp.feature_density = genome_ok ? gn.feature_density : 0.5f;
    pp.band_detail     = genome_ok ? gn.band_detail     : 0.5f;
    pp.cap_extent      = genome_ok ? gn.cap_extent      : 0.2f;
    pp.roughness       = genome_ok ? gn.roughness       : 0.5f;
    pp.anomaly         = (f32)gn.anomaly;
    // Seasonal ring shadow: give the planet a deterministic axial tilt (obliquity eps) and node
    // from its seed, then the star's elevation above the ring plane = sin(eps)*cos(orbit_angle -
    // node). This sweeps the planet's shadow across the rings as it orbits (a long thin band near
    // equinox, retracted near solstice). Only visible on ringed planets; harmless otherwise.
    f32 eps  = 0.18f + 0.40f * (seed * 0.113f - floorf(seed * 0.113f));   // ~10..33 deg tilt
    f32 node = 6.2831853f * (seed * 0.317f - floorf(seed * 0.317f));
    pp.ring_shadow_elev = sinf(eps) * cosf(planet.orbit_angle - node);
    pp.star_color  = star_color;
    pp.layer       = layer;
    pp.fb_w = fb_w; pp.fb_h = fb_h;
    renderer_draw_planetsurface(&pp);
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
    // Save-on-change: StarFxSystem::shutdown() has no caller (the engine boundary has no
    // game-shutdown callback), so a shutdown-time save never runs. Persist the moment a
    // persisted tunable moves instead — a slider drag rewrites the small cfg per tick.
    f32 g_before[8] = { godray_intensity, godray_density, godray_decay, godray_exposure,
                        godray_halo, godray_transmission, backlit_transmission, backlit_rim };
    bs_ui_text_colored(SP[0], SP[1], SP[2], SP[3], "Sun Shafts (God Rays)");
    bs_ui_slider_float("Shaft intensity",   &godray_intensity,     0.0f, 3.0f);
    bs_ui_slider_float("Shaft density",     &godray_density,       0.1f, 1.0f);
    bs_ui_slider_float("Shaft decay",       &godray_decay,         0.85f, 1.0f);
    bs_ui_slider_float("Shaft exposure",    &godray_exposure,      0.0f, 4.0f);
    bs_ui_slider_float("Source halo",       &godray_halo,          0.5f, 8.0f);
    bs_ui_slider_float("Hull leak",         &godray_transmission,  0.0f, 1.0f);
    bs_ui_text_colored(SP[0], SP[1], SP[2], SP[3], "Hull Backlight");
    bs_ui_slider_float("Backlit glow",      &backlit_transmission, 0.0f, 3.0f);
    bs_ui_slider_float("Backlit rim",       &backlit_rim,          0.0f, 3.0f);
    f32 g_after[8] = { godray_intensity, godray_density, godray_decay, godray_exposure,
                       godray_halo, godray_transmission, backlit_transmission, backlit_rim };
    if (memcmp(g_before, g_after, sizeof(g_before)) != 0) planet_params_save();
    bs_ui_text_colored(SP[0], SP[1], SP[2], SP[3], "Star Rendering");
    bool mode_3d = star_3d_mode ? true : false;
    bs_ui_checkbox("3D sphere mode", &mode_3d);
    star_3d_mode = mode_3d ? TRUE : FALSE;
    if (star_3d_mode)
    {
        bs_ui_slider_float("Sphere radius",  &star_body_scale,     0.5f, 60.0f);
        bs_ui_slider_float("Rotation speed", &star_rotation_speed, 0.0f, 30.0f);
        bs_ui_text_colored(SP[0], SP[1], SP[2], SP[3], "Surface (GPU)");
        bs_ui_slider_float("Noise scale",      &surf_noise_scale,      1.0f, 20.0f);
        bs_ui_slider_float("Flow speed",       &surf_flow_speed,       0.0f, 1.0f);
        bs_ui_slider_float("Granule contrast", &surf_granule_contrast, 0.0f, 3.0f);
        bs_ui_slider_float("Hotspot gain",     &surf_hotspot_gain,     0.0f, 2.0f);
        bs_ui_slider_float("Sunspot density",  &surf_sunspot_density,  0.0f, 1.0f);
        bs_ui_slider_float("Limb darkening",   &surf_limb_darkening,   0.1f, 2.0f);
        bs_ui_slider_float("Brightness",       &surf_brightness,       0.2f, 2.0f);
        bs_ui_slider_float("Rim brightness",   &surf_corona_strength,  0.0f, 2.0f);
        bs_ui_slider_float("Black radius",     &surf_dark_radius,      1.0f, 16.0f);
    }
}

// ---- Per-type planet appearance ------------------------------------------------------
static_assert((i32)PLANET_TYPE_COUNT == StarFxSystem::PLANET_EDITOR_TYPE_COUNT,
              "PlanetTypeParams table size must match the PlanetType enum count");

void StarFxSystem::planet_params_reset_defaults()
{
    // Base colors mirror ss_generation.cpp wg_planet_base_color. size_mul scales the physical
    // radius so planets grow with zoom exactly like the star (star_body_scale); giants read a
    // touch larger. min_px is the zoomed-out on-screen floor (mirrors STAR_MIN_SCREEN_RADIUS).
    struct Def { f32 size_mul, min_px, rot, halo, cloud, r, g, b; };
    static const Def D[PLANET_EDITOR_TYPE_COUNT] = {
        /* LAVA      */ { 6.0f, 4.0f, 0.03f, 1.0f, 0.00f, 0.78f, 0.24f, 0.12f },
        /* ROCKY     */ { 6.0f, 4.0f, 0.03f, 1.0f, 0.00f, 0.52f, 0.47f, 0.42f },
        /* DESERT    */ { 6.0f, 4.0f, 0.03f, 1.0f, 0.00f, 0.82f, 0.66f, 0.40f },
        /* OCEAN     */ { 6.0f, 4.0f, 0.03f, 1.0f, 0.55f, 0.20f, 0.46f, 0.78f },
        /* TERRAN    */ { 6.0f, 4.0f, 0.03f, 1.0f, 0.55f, 0.32f, 0.60f, 0.42f },
        /* GAS_GIANT */ { 8.0f, 5.0f, 0.03f, 1.0f, 0.00f, 0.80f, 0.66f, 0.46f },
        /* ICE_GIANT */ { 7.0f, 5.0f, 0.03f, 1.0f, 0.00f, 0.48f, 0.70f, 0.86f },
        /* FROZEN    */ { 6.0f, 4.0f, 0.03f, 1.0f, 0.00f, 0.82f, 0.86f, 0.94f },
    };
    for (i32 i = 0; i < PLANET_EDITOR_TYPE_COUNT; ++i) {
        planet_params[i].size_mul       = D[i].size_mul;
        planet_params[i].min_px         = D[i].min_px;
        planet_params[i].rotation_speed = D[i].rot;
        planet_params[i].halo_scale     = D[i].halo;
        planet_params[i].cloud_amount   = D[i].cloud;
        planet_params[i].surface_color  = bs_color{ D[i].r, D[i].g, D[i].b, 1.0f };
    }
}

b8 StarFxSystem::planet_params_save() const
{
    FILE* f = fopen("planet_editor.cfg", "w");
    if (!f) return FALSE;
    fprintf(f, "# blackstride planet editor v1: idx size_mul min_px rot halo cloud r g b\n");
    for (i32 i = 0; i < PLANET_EDITOR_TYPE_COUNT; ++i) {
        const PlanetTypeParams& p = planet_params[i];
        fprintf(f, "%d %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f\n",
                i, p.size_mul, p.min_px, p.rotation_speed, p.halo_scale, p.cloud_amount,
                p.surface_color.r, p.surface_color.g, p.surface_color.b);
    }
    // God rays + hull backlight ("g" line; older loaders skip it — sscanf %d rejects 'g').
    fprintf(f, "g %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f\n",
            godray_intensity, godray_density, godray_decay, godray_exposure,
            godray_halo, godray_transmission, backlit_transmission, backlit_rim);
    fclose(f);
    return TRUE;
}

b8 StarFxSystem::planet_params_load()
{
    FILE* f = fopen("planet_editor.cfg", "r");
    if (!f) return FALSE;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        if (line[0] == 'g') {
            // God rays + hull backlight. Tolerate short lines from older files: only the
            // values actually parsed overwrite their defaults.
            float v[8];
            i32 n = sscanf(line + 1, "%f %f %f %f %f %f %f %f",
                           &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
            f32* dst[8] = { &godray_intensity, &godray_density, &godray_decay,
                            &godray_exposure, &godray_halo, &godray_transmission,
                            &backlit_transmission, &backlit_rim };
            for (i32 i = 0; i < n && i < 8; ++i) *dst[i] = v[i];
            continue;
        }
        i32 idx = -1;
        float sm, mp, rs, hs, ca, r, g, b;
        if (sscanf(line, "%d %f %f %f %f %f %f %f %f",
                   &idx, &sm, &mp, &rs, &hs, &ca, &r, &g, &b) == 9 &&
            idx >= 0 && idx < PLANET_EDITOR_TYPE_COUNT) {
            PlanetTypeParams& p = planet_params[idx];
            p.size_mul = sm; p.min_px = mp; p.rotation_speed = rs;
            p.halo_scale = hs; p.cloud_amount = ca;
            p.surface_color = bs_color{ r, g, b, 1.0f };
        }
    }
    fclose(f);
    return TRUE;
}

void StarFxSystem::build_planet_editor()
{
    if (bs_ui_begin_window("PLANET EDITOR", &show_planet_editor)) {
        const f32 H[4] = { 0.55f, 0.85f, 0.95f, 1.0f };
        bs_ui_text_colored(H[0], H[1], H[2], H[3], "PER-TYPE PLANET APPEARANCE");
        bs_ui_text("Size is a radius multiplier; planets grow with zoom like the star.");
        bs_ui_separator();
        bool use_genome = planet_use_genome_colors ? true : false;
        bs_ui_checkbox("Use genome colors (per-planet)", &use_genome);
        planet_use_genome_colors = use_genome ? TRUE : FALSE;
        bs_ui_combo("Planet type", &planet_editor_sel_type,
                    "Lava\0Rocky\0Desert\0Ocean\0Terran\0Gas Giant\0Ice Giant\0Frozen\0");
        if (planet_editor_sel_type < 0) planet_editor_sel_type = 0;
        if (planet_editor_sel_type >= PLANET_EDITOR_TYPE_COUNT)
            planet_editor_sel_type = PLANET_EDITOR_TYPE_COUNT - 1;
        PlanetTypeParams& p = planet_params[planet_editor_sel_type];
        bs_ui_separator();
        bs_ui_slider_float("Size (radius x)", &p.size_mul,       0.5f, 30.0f);
        bs_ui_slider_float("Min size (px)",   &p.min_px,         1.0f, 30.0f);
        bs_ui_slider_float("Rotation speed",  &p.rotation_speed, 0.0f, 3.0f);
        bs_ui_slider_float("Atmo/ring size",  &p.halo_scale,     0.5f, 3.0f);
        bs_ui_slider_float("Clouds",          &p.cloud_amount,   0.0f, 1.0f);
        f32 col[3] = { p.surface_color.r, p.surface_color.g, p.surface_color.b };
        if (bs_ui_color_edit3("Surface color", col)) {
            p.surface_color.r = col[0];
            p.surface_color.g = col[1];
            p.surface_color.b = col[2];
        }
        bs_ui_separator();
        if (bs_ui_button("Save to disk", TRUE))          planet_params_save();
        if (bs_ui_button("Reload from disk", TRUE))      planet_params_load();
        if (bs_ui_button("Reset all to defaults", TRUE)) planet_params_reset_defaults();
    }
    bs_ui_end_window();
}
