#include "out_sensor_detection_fx.h"
#include <math.h>
#include <renderer/renderer.h>
using namespace bs_math;

static const u32 LAYER_FX = 15; // above ships and celestial, below UI

static f32 sensor_visibility_from_dist(f32 dist, f32 range) {
    if (range <= 0.0f) return 1.0f;
    if (dist >= range) return 0.0f;
    f32 t = dist / range;
    return 1.0f - t * t * t;
}

// Submit a single thick line segment as a glowing sprite. The glow is driven by
// bs_sprite.custom.w (overlay glow) rather than custom.x (heat/distortion), so the
// overlay stays clean.
static void draw_glow_line(const bs_glow_params* glow, Vec2 a, Vec2 b, f32 thickness,
                           bs_color color, u32 layer, f32 zoom)
{
    Vec2 d = vec2_sub(b, a);
    f32 len = vec2_length(d);
    if (len <= 0.0001f) return;
    f32 world_thickness = thickness / zoom;
    bs_sprite s{};
    s.position = Vec2{ (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f };
    s.size     = Vec2{ len, world_thickness };
    s.origin   = Vec2{ 0.5f, 0.5f };
    s.rotation = atan2f(d.y, d.x);
    s.uv       = bs_rect{ 0.0f, 0.0f, 1.0f, 1.0f };
    s.tint     = color;
    s.custom   = bs_color{ 0.0f, 0.0f, 0.0f, 1.0f }; // custom.w enables overlay glow
    s.texture  = bs_texture{ 0 };
    s.blend    = BLEND_ADDITIVE;
    s.layer    = layer;
    s.glow_override = glow;
    renderer_draw_sprite(&s);
}

// Draw a glowing circle outline by submitting a chain of glowing line segments.
static void draw_glow_circle(const bs_glow_params* glow, Vec2 center, f32 radius,
                             u32 segments, f32 thickness, bs_color color,
                             u32 layer, f32 zoom)
{
    if (radius <= 0.0f) return;
    if (segments < 6u)  segments = 6u;
    if (segments > 256u) segments = 256u;
    const f32 two_pi = 2.0f * BS_PI;
    Vec2 prev{ center.x + radius, center.y };
    for (u32 i = 1; i <= segments; ++i) {
        f32 t = (f32)i / (f32)segments * two_pi;
        Vec2 cur{ center.x + cosf(t) * radius, center.y + sinf(t) * radius };
        draw_glow_line(glow, prev, cur, thickness, color, layer, zoom);
        prev = cur;
    }
}

static void reset_overlay_glow(bs_glow_params* g) {
    g->intensity = 1.5f;
    g->falloff = 3.0f;
    g->head_mult = 0.0f;
    g->head_falloff = 1.0f;
    g->head_range = 0.0f;
    g->distort_amp = 0.0f;
    g->wave_speed = 0.0f;
    g->wave_freq = 0.0f;
    g->jitter_speed = 0.0f;
    g->jitter_freq = 0.0f;
    g->glow_tint = bs_color{ 1.0f, 1.0f, 1.0f, 1.0f };
    g->temp_cool = bs_color{ 1.0f, 1.0f, 1.0f, 1.0f };
    g->temp_warm = bs_color{ 1.0f, 1.0f, 1.0f, 1.0f };
    g->temp_hot = bs_color{ 1.0f, 1.0f, 1.0f, 1.0f };
}

void OutSensorDetectionFX::init() {
    color = bs_color{ 1.0f, 0.45f, 0.45f, 1.0f };
    intensity = 1.0f;
    radius_scale = 0.6f;
    reset_overlay_glow(&overlay_glow);
    sweep_speed = 1.5f;
    sweep_angle = 0.0f;
    ping_phase = 0.0f;
}

void OutSensorDetectionFX::update(f32 dt) {
    sweep_angle += sweep_speed * dt;
    while (sweep_angle > 2.0f * BS_PI) sweep_angle -= 2.0f * BS_PI;
    ping_phase += dt * 0.5f;
    while (ping_phase > 1.0f) ping_phase -= 1.0f;
}

void OutSensorDetectionFX::render(f32 elapsed_time, const Ship* enemy, const Ship* player, f32 sensor_range, f32 zoom) {
    (void)elapsed_time;
    if (!enemy || !player) return;

    Vec2 delta = hierpos_diff(&enemy->origin, &player->origin, BS_HIERPOS_CELL_SIZE);
    f32 dist = vec2_length(delta);
    f32 vis = sensor_visibility_from_dist(dist, sensor_range);
    f32 outside = 1.0f - vis;
    if (outside <= 0.001f) return;

    Vec2 rp = enemy->render_pos;  // transient render-space position set by the ship render pass

    // Scale the overlay so it stays the same on-screen size regardless of zoom.
    f32 screen_scale = clampf(1.0f / zoom, 1.0f, 8.0f);
    f32 base_radius = ship_bounding_radius(enemy) * radius_scale;
    f32 radius = base_radius * screen_scale;

    bs_color fx_col = color; // full intensity, independent of distance
    fx_col.r *= intensity;
    fx_col.g *= intensity;
    fx_col.b *= intensity;
    fx_col.a *= intensity;

    // Outer radar ring: soft thick glow + crisp thin edge.
    bs_color glow_col = fx_col;
    glow_col.a *= 0.35f;
    draw_glow_circle(&overlay_glow, rp, radius, 48, 8.0f * screen_scale, glow_col, LAYER_FX, zoom);
    draw_glow_circle(&overlay_glow, rp, radius, 48, 2.5f * screen_scale, fx_col, LAYER_FX + 1, zoom);

    // Expanding ping ring.
    if (ping_phase < 1.0f) {
        bs_color ping_col = fx_col;
        ping_col.a *= 1.0f - ping_phase;
        draw_glow_circle(&overlay_glow, rp, radius * ping_phase, 48, 4.0f * screen_scale, ping_col, LAYER_FX + 1, zoom);
    }

    // Rotating sweep wedge (fan of radial lines).
    const i32 WEDGE_SEGMENTS = 10;
    const f32 WEDGE_HALF_ANGLE = 0.35f; // ~20 degrees
    for (i32 i = 0; i <= WEDGE_SEGMENTS; ++i) {
        f32 t = (f32)i / (f32)WEDGE_SEGMENTS;
        f32 angle = sweep_angle - WEDGE_HALF_ANGLE + t * (2.0f * WEDGE_HALF_ANGLE);
        Vec2 end = vec2_add(rp,
                            vec2_rotate(Vec2{ 0.0f, radius }, angle));
        bs_color wedge_col = fx_col;
        // Brightest in the middle of the wedge, fading toward the edges.
        f32 edge = fabsf(t - 0.5f) * 2.0f;
        wedge_col.a *= 0.5f + 0.5f * (1.0f - edge);
        draw_glow_line(&overlay_glow, rp, end, 3.0f * screen_scale, wedge_col, LAYER_FX + 2, zoom);
    }

    // Central crosshair.
    f32 cross = 10.0f * screen_scale;
    bs_color cross_col = fx_col;
    cross_col.a *= 0.9f;
    draw_glow_line(&overlay_glow, Vec2{ rp.x - cross, rp.y },
                   Vec2{ rp.x + cross, rp.y },
                   2.0f * screen_scale, cross_col, LAYER_FX + 3, zoom);
    draw_glow_line(&overlay_glow, Vec2{ rp.x, rp.y - cross },
                   Vec2{ rp.x, rp.y + cross },
                   2.0f * screen_scale, cross_col, LAYER_FX + 3, zoom);

    // Central contact blip.
    f32 blip = 12.0f * screen_scale;
    bs_sprite sp{};
    sp.position = rp;
    sp.size     = Vec2{ blip, blip };
    sp.origin   = Vec2{ 0.5f, 0.5f };
    sp.rotation = 0.0f;
    sp.uv       = bs_rect{ 0.0f, 0.0f, 1.0f, 1.0f };
    sp.tint     = fx_col;
    sp.texture  = bs_texture{ 0 };
    sp.blend    = BLEND_ADDITIVE;
    sp.layer    = LAYER_FX + 4;
    sp.custom   = bs_color{ 0.0f, 0.0f, 0.0f, 1.0f }; // custom.w enables overlay glow
    sp.glow_override = &overlay_glow;
    renderer_draw_sprite(&sp);
}
