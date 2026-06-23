#include "projectile.h"
#include <renderer/renderer.h>
#include <math.h>
using namespace bs_math;
static f32 smoothstep(f32 edge0, f32 edge1, f32 x) {
    f32 t = (x - edge0) / (edge1 - edge0);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}
void ProjectileSystem::init() {
    for (i32 i = 0; i < MAX_PROJECTILES; ++i)
        pool[i].active = FALSE;
    count = 0;
    // ---- 2D tapered bullet streak (128 x 512) -------------------------------------------
    // Y=0 is the transparent tail, Y=511 is the bright head.
    // Widthwise Gaussian falloff (sigma=8) for a sharp taper to a thin line.
    // A hot white core line (centre 4 pixels) runs down the middle.
    {
        u8 pixels[128 * 512 * 4];
        f32 cx = 64.0f;
        f32 sigma = 8.0f;
        for (i32 y = 0; y < 512; ++y) {
            f32 len_t = smoothstep(0.0f, 1.0f, (f32)y / 511.0f); // head opacity
            for (i32 x = 0; x < 128; ++x) {
                f32 dx = (f32)x - cx;
                f32 gauss = expf(-(dx * dx) / (2.0f * sigma * sigma));
                f32 alpha = len_t * gauss;
                u8 a = (u8)(alpha * 255.0f);
                // hot white core line in the centre 4 pixels
                b8 core = (x >= 62 && x <= 65);
                u8 r = core ? 255 : 255;
                u8 g = core ? 255 : (u8)(200 + 55.0f * alpha);
                u8 b = core ? 255 : (u8)(100 + 155.0f * alpha);
                i32 idx = (y * 128 + x) * 4;
                pixels[idx + 0] = r;
                pixels[idx + 1] = g;
                pixels[idx + 2] = b;
                pixels[idx + 3] = a;
            }
        }
        streak_texture = renderer_create_texture(pixels, 128, 512);
    }
    // ---- Radial flash gradient (128 x 128) ----------------------------------------------
    // Strong falloff (sigma=12) so alpha ≈ 0 at texture edges — no square outline.
    {
        u8 pixels[128 * 128 * 4];
        f32 cx = 64.0f, cy = 64.0f;
        f32 sigma = 12.0f;
        for (i32 y = 0; y < 128; ++y) {
            for (i32 x = 0; x < 128; ++x) {
                f32 dx = (f32)x - cx;
                f32 dy = (f32)y - cy;
                f32 dist2 = dx * dx + dy * dy;
                f32 alpha = expf(-dist2 / (2.0f * sigma * sigma));
                u8 a = (u8)(alpha * 255.0f);
                i32 idx = (y * 128 + x) * 4;
                pixels[idx + 0] = 255;
                pixels[idx + 1] = 255;
                pixels[idx + 2] = 255;
                pixels[idx + 3] = a;
            }
        }
        flash_texture = renderer_create_texture(pixels, 128, 128);
    }
}
b8 ProjectileSystem::spawn(Vec2 origin, Vec2 velocity,
                           f32 lifetime, f32 radius, bs_color color, VesselFaction owner)
{
    // find first free slot
    for (i32 i = 0; i < MAX_PROJECTILES; ++i) {
        if (!pool[i].active) {
            pool[i].active   = TRUE;
            pool[i].position = origin;
            pool[i].velocity = velocity;
            pool[i].lifetime = lifetime;
            pool[i].age      = 0.0f;
            pool[i].radius   = radius;
            pool[i].color    = color;
            pool[i].owner    = owner;
            ++count;
            return TRUE;
        }
    }
    return FALSE; // pool full
}
void ProjectileSystem::update(f32 dt) {
    i32 active_count = 0;
    for (i32 i = 0; i < MAX_PROJECTILES; ++i) {
        Projectile& p = pool[i];
        if (!p.active) continue;
        p.position = vec2_add(p.position, vec2_scale(p.velocity, dt));
        p.lifetime -= dt;
        p.age      += dt;
        if (p.lifetime <= 0.0f) {
            p.active = FALSE;
        } else {
            ++active_count;
        }
    }
    count = active_count;
}
void ProjectileSystem::render(u32 layer) const {
    for (i32 i = 0; i < MAX_PROJECTILES; ++i) {
        const Projectile& p = pool[i];
        if (!p.active) continue;
        f32 speed = vec2_length(p.velocity);
        f32 trail_length = speed * 0.04f;
        if (trail_length < p.radius * 4.0f)
            trail_length = p.radius * 4.0f;
        // ---- Streak (velocity-aligned) ------------------------------------------------------
        bs_sprite streak{};
        streak.position = p.position;
        streak.size     = Vec2{ p.radius * 3.0f, trail_length };
        streak.origin   = Vec2{ 0.5f, 1.0f }; // anchor bottom edge (head) at position
        streak.rotation = atan2f(p.velocity.y, p.velocity.x) + 3.14159265f / 2.0f;
        streak.uv       = bs_rect{ 0.0f, 0.0f, 1.0f, 1.0f };
        streak.tint     = p.color;
        streak.custom   = bs_color{ 1.0f, p.age, 0.0f, 0.0f }; // x=glow, y=age(shimmer)
        streak.texture       = streak_texture;
        streak.blend         = BLEND_ADDITIVE;
        streak.layer           = layer;
        streak.glow_override = glow_override;
        renderer_draw_sprite(&streak);
        // ---- Muzzle flash (large burst, only first 50 ms) -----------------------------------
        if (p.age < 0.05f) {
            f32 flash_alpha = 1.0f - (p.age / 0.05f);
            f32 flash_scale = 4.0f + 6.0f * flash_alpha;
            bs_sprite flash{};
            flash.position = p.position;
            flash.size     = Vec2{ p.radius * flash_scale, p.radius * flash_scale };
            flash.origin   = Vec2{ 0.5f, 0.5f };
            flash.rotation = 0.0f;
            flash.uv       = bs_rect{ 0.0f, 0.0f, 1.0f, 1.0f };
            flash.tint     = bs_color{ 1.0f, 0.85f, 0.5f, flash_alpha };
            flash.custom   = bs_color{ flash_alpha, p.age, 0.0f, 0.0f };
            flash.texture       = flash_texture;
            flash.blend         = BLEND_ADDITIVE;
            flash.layer           = layer;
            flash.glow_override = glow_override;
            renderer_draw_sprite(&flash);
        }
    }
}
