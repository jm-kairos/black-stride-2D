#include "sim/projectile.h"
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
    // Left null here: combat_arena_init points it at the game_state member right after this
    // call. A ProjectileSystem with no FX sink is fully functional -- it just draws nothing
    // at launch or termination.
    fx = nullptr;

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
b8 ProjectileSystem::spawn(HierPos2 origin, Vec2 velocity,
                           f32 lifetime, f32 radius, bs_color color, VesselFaction owner, i16 faction_id,
                           f32 radiation_emission, f32 hp, u8 kind, f32 dmg, u8 vfx_family)
{
    // find first free slot
    for (i32 i = 0; i < MAX_PROJECTILES; ++i) {
        if (!pool[i].active) {
            pool[i].active             = TRUE;
            pool[i].kind               = kind;
            pool[i].position           = origin;
            pool[i].velocity           = velocity;
            pool[i].lifetime           = lifetime;
            pool[i].age                = 0.0f;
            pool[i].radius             = radius;
            pool[i].color              = color;
            pool[i].owner              = owner;
            pool[i].faction_id         = faction_id;
            pool[i].radiation_emission = radiation_emission;
            pool[i].hp                 = hp;
            pool[i].max_hp             = hp;
            pool[i].dmg                = dmg;
            pool[i].vfx_family         = vfx_family;
            pool[i].trail_count        = 0;      // a reused slot must not inherit the last
            pool[i].trail_timer        = 0.0f;   // occupant's flight path

            pool[i].max_speed          = 0.0f;   // shells are unguided: no per-projectile clamp
            ++count;
            // Muzzle flash, pinned in world space at the barrel. `velocity` carries the firing
            // ship's own velocity too, but a shell leaves at 8500-16000 units/s against a hull
            // doing a few hundred, so its direction is the barrel axis to well within what a
            // 75 ms flash can show.
            if (fx)
                fx->emit(PFX_MUZZLE, vfx_family, origin, velocity, radius,
                         projectile_fx_power(dmg), color);
            return TRUE;
        }
    }
    return FALSE; // pool full
}
b8 ProjectileSystem::spawn_missile(HierPos2 origin, Vec2 velocity,
                                   f32 lifetime, f32 radius, bs_color color, VesselFaction owner,
                                   i16 faction_id, f32 radiation_emission, f32 hp, f32 dmg,
                                   f32 max_speed, u8 vfx_family)
{
    // Same free-slot scan as spawn(), but the slot is tagged PROJ_MISSILE so the combat-arena
    // steering pass picks it up. Kept as a separate loop (not a spawn() call) because spawn()
    // does not report which slot it filled.
    for (i32 i = 0; i < MAX_PROJECTILES; ++i) {
        if (!pool[i].active) {
            pool[i].active             = TRUE;
            pool[i].kind               = PROJ_MISSILE;
            pool[i].position           = origin;
            pool[i].velocity           = velocity;
            pool[i].lifetime           = lifetime;
            pool[i].age                = 0.0f;
            pool[i].radius             = radius;
            pool[i].color              = color;
            pool[i].owner              = owner;
            pool[i].faction_id         = faction_id;
            pool[i].radiation_emission = radiation_emission;
            pool[i].hp                 = hp;
            pool[i].max_hp             = hp;
            pool[i].dmg                = dmg;
            pool[i].vfx_family         = vfx_family;
            pool[i].trail_count        = 0;      // a reused slot must not inherit the last
            pool[i].trail_timer        = 0.0f;   // occupant's flight path

            pool[i].max_speed          = max_speed;
            ++count;
            // Same launch flash as a shell. A launcher is a tube rather than a barrel, but the
            // event being drawn -- ordnance leaving the hull -- is the same one, and reusing it
            // means a new WeaponKind inherits the flash without touching this file.
            if (fx)
                fx->emit(PFX_MUZZLE, vfx_family, origin, velocity, radius,
                         projectile_fx_power(dmg), color);
            return TRUE;
        }
    }
    return FALSE; // pool full
}
void ProjectileSystem::retire(i32 index, u8 fx_kind, f32 fx_scale) {
    if (index < 0 || index >= MAX_PROJECTILES) return;
    Projectile& p = pool[index];
    if (!p.active) return;   // idempotent: a slot freed twice must not double-decrement
    if (fx)
        fx->emit(fx_kind, p.vfx_family, p.position, p.velocity,
                 (fx_scale > 0.0f) ? fx_scale : p.radius,
                 projectile_fx_power(p.dmg), p.color);
    p.active = FALSE;
    --count;
}
void ProjectileSystem::update(f32 dt) {
    i32 active_count = 0;
    for (i32 i = 0; i < MAX_PROJECTILES; ++i) {
        Projectile& p = pool[i];
        if (!p.active) continue;
        Vec2 step = vec2_scale(p.velocity, dt);
        p.position = hierpos_add_vec2(&p.position, step);
        p.lifetime -= dt;
        p.age      += dt;
        // ---- Curved-trail history (cosmetic; guided rounds only) -------------------------
        // A shell flies a straight line, so the single stretched quad the renderer draws for it
        // is exactly right and no history is worth paying for. A MISSILE is steered every tick
        // by the combat-arena guidance pass, so a straight streak actively misreports where it
        // has been -- the harder it turns, the more wrong the picture. This is the one place the
        // reference document's position-history trail earns its cost.
        if (p.kind == PROJ_MISSILE) {
            // Rebase: every stored offset is relative to `position`, which just moved.
            for (i32 k = 0; k < p.trail_count; ++k)
                p.trail[k] = vec2_sub(p.trail[k], step);
            p.trail_timer -= dt;
            if (p.trail_timer <= 0.0f) {
                i32 last = (p.trail_count < PROJ_TRAIL_SAMPLES) ? p.trail_count
                                                               : PROJ_TRAIL_SAMPLES - 1;
                for (i32 k = last; k > 0; --k) p.trail[k] = p.trail[k - 1];
                p.trail[0] = Vec2{ 0.0f, 0.0f };   // newest sample IS the current position
                if (p.trail_count < PROJ_TRAIL_SAMPLES) ++p.trail_count;
                p.trail_timer = PROJ_TRAIL_INTERVAL;
            }
        }
        if (p.lifetime <= 0.0f) {
            p.active = FALSE;
        } else {
            ++active_count;
        }
    }
    count = active_count;
}
void ProjectileSystem::render(u32 layer, const bs_math::HierPos2* camera,
                              const bs_glow_params* const* glow_by_family) const {
    for (i32 i = 0; i < MAX_PROJECTILES; ++i) {
        const Projectile& p = pool[i];
        if (!p.active) continue;
        bs_math::Vec2 draw_pos = hierpos_diff(&p.position, camera);
        f32 speed = vec2_length(p.velocity);
        // Every sprite this round submits shares one glow pointer, so a family costs one draw
        // run rather than one per sprite (the backend breaks runs on pointer identity).
        const bs_glow_params* glow_override =
            glow_by_family ? glow_by_family[(p.vfx_family < 3) ? p.vfx_family : 0] : nullptr;
        // ---- Per-family travel geometry -----------------------------------------------------
        // Three visual languages, keyed off the firing weapon's authored vfx_family. What varies
        // is how the round advertises what it IS:
        //
        //   SHELL     an inert propellant round -- a hot streak whose length reads as its speed.
        //   SLUG      a rail-driven round -- LONGER and much NARROWER. The reference doc's
        //             section 3.3 makes exactly this point: a railgun round can be extremely
        //             fast while staying visually tiny, and widening it with speed would say
        //             "big" when the thing to say is "fast".
        //   ORDNANCE  a powered object -- the SHORTEST streak of the three, because a missile at
        //             5-10k u/s is not leaving a hypervelocity trail; what the player should read
        //             is the engine, which is drawn separately below. It also does not use the
        //             straight streak at all once it has flown long enough to have a history:
        //             see the segment chain below.
        f32 len_mul, wid_mul, head_mul;
        switch (p.vfx_family) {
            case 1:  len_mul = 0.075f; wid_mul = 1.6f; head_mul = 3.2f; break;  // VFX_SLUG
            case 2:  len_mul = 0.018f; wid_mul = 2.2f; head_mul = 5.0f; break;  // VFX_ORDNANCE
            default: len_mul = 0.040f; wid_mul = 3.0f; head_mul = 4.5f; break;  // VFX_SHELL
        }
        f32 trail_length = speed * len_mul;
        if (trail_length < p.radius * 4.0f)
            trail_length = p.radius * 4.0f;
        // ---- Curved trail (guided rounds with enough history) --------------------------------
        // A chain of short quads threaded through the recorded positions. This exists because a
        // missile TURNS: the straight velocity-aligned streak below points along the CURRENT
        // heading and therefore draws a path the round never flew, and the error is largest
        // exactly when the missile is doing the interesting thing -- pulling onto a target.
        //
        // Section 3.2 asks for fade by both distance from the projectile and age of the segment.
        // Index does both jobs here, because samples are taken on a fixed clock: segment k is
        // both the k-th oldest and the k-th furthest back.
        //
        // Cost is 7 quads per guided round, and it is affordable only because it is exclusive to
        // guided rounds. Applied to the 512-slot pool this technique would be ~3,500 sprites of
        // a 16,384 frame budget; applied to missiles it is a few dozen.
        const b8 curved = (p.vfx_family == 2 && p.trail_count >= 2);
        if (curved) {
            const f32 rot_off = 3.14159265f / 2.0f;
            for (i32 k = 0; k + 1 < p.trail_count; ++k) {
                Vec2 a  = p.trail[k];       // nearer the round
                Vec2 b  = p.trail[k + 1];   // further back
                Vec2 d  = vec2_sub(b, a);
                f32  seg = vec2_length(d);
                if (seg < 1.0e-3f) continue;   // stationary tick: nothing to draw
                f32 u0 = (f32)k / (f32)(PROJ_TRAIL_SAMPLES - 1);
                f32 fade = (1.0f - u0) * (1.0f - u0);   // squared: bright at the round, gone at the tail
                bs_sprite seg_sp{};
                // Anchor the segment's NEAR end at its own sample and let it run backwards, the
                // same origin/rotation convention the straight streak uses, so the chain lines up
                // with the head instead of straddling it.
                //
                // WIDTH is the chain's own, not the streak's `wid_mul`. At combat framing a 6-unit
                // missile drawn at the streak's 2.2x is ~1.5 px across, and a Gaussian cross-section
                // that narrow rasterises to a dashed line -- the arc was there and unreadable. The
                // chain needs enough width to survive as a continuous stroke; the taper then does
                // the work of showing which end is the round.
                const f32 CHAIN_WIDTH = 4.6f;
                seg_sp.position = vec2_add(draw_pos, a);
                seg_sp.size     = Vec2{ p.radius * CHAIN_WIDTH * (0.30f + 0.70f * (1.0f - u0)), seg };
                seg_sp.origin   = Vec2{ 0.5f, 1.0f };
                seg_sp.rotation = atan2f(-d.y, -d.x) + rot_off;
                // A thin slice near the streak texture's bright end: the full texture tapers
                // along its length, which is correct for one streak and wrong for a link in a
                // chain. The slice gives a soft-edged bar of near-constant brightness.
                seg_sp.uv       = bs_rect{ 0.0f, 0.85f, 1.0f, 0.10f };
                seg_sp.tint     = bs_color{ p.color.r, p.color.g, p.color.b, 1.0f * fade };
                seg_sp.custom   = bs_color{ 0.0f, 0.0f, 1.0f, 0.70f * fade };
                seg_sp.texture       = streak_texture;
                seg_sp.blend         = BLEND_ADDITIVE;
                seg_sp.layer         = layer;
                seg_sp.glow_override = glow_override;
                renderer_draw_sprite(&seg_sp);
            }
        }
        // ---- Streak (velocity-aligned) ------------------------------------------------------
        bs_sprite streak{};
        streak.position = draw_pos;
        streak.size     = Vec2{ p.radius * wid_mul, trail_length };
        streak.origin   = Vec2{ 0.5f, 1.0f }; // anchor bottom edge (head) at position
        streak.rotation = atan2f(p.velocity.y, p.velocity.x) + 3.14159265f / 2.0f;
        streak.uv       = bs_rect{ 0.0f, 0.0f, 1.0f, 1.0f };
        streak.tint     = p.color;
        // x=glow (drives heat distortion + the white-hot-head/red-tail temperature ramp),
        // y=age (shimmer phase), z=1 marks the sprite SELF-EMISSIVE. The z flag is what lets
        // shots sit on LAYER_PROJECTILE (below the engine's unlit threshold, so they go
        // through bloom) without the map look's star light and bright ambient shading them:
        // the shader skips scene lighting entirely when custom.z >= 0.5. Without it, moving
        // off LAYER_UI would have dimmed every shot in the galaxy-map look.
        streak.custom   = bs_color{ 1.0f, p.age, 1.0f, 0.0f };
        streak.texture       = streak_texture;
        streak.blend         = BLEND_ADDITIVE;
        streak.layer           = layer;
        streak.glow_override = glow_override;
        // Suppressed once the curved chain has taken over, or the two would overlap and the
        // straight one would poke out of the arc on every turn -- the exact artefact the chain
        // exists to remove. A guided round still gets the streak for its first ~50 ms, before
        // enough samples exist, which is also when it is genuinely still flying straight.
        if (!curved) renderer_draw_sprite(&streak);
        // ---- Incandescent head -------------------------------------------------------------
        // A tight bright orb pinned at the shot's leading point. The streak alone tapers to a
        // line and reads as a scratch on the screen; the head is what makes it read as a solid
        // object with a hot face, and it is what the bloom pass has to bite on.
        //
        // This REPLACES the old age < 0.05 s flash that used to be drawn here. That effect was
        // nominally a muzzle flash but rode the projectile away from the barrel, so it always
        // detached from the gun that fired it. The real muzzle flash is now a world-pinned
        // ProjectileFx event emitted in spawn(), which stays where the shot actually left.
        //
        // Budget: this makes the travel component 2 sprites per live shot (3 for guided
        // ordnance, which also burns a plume) against the shared 16384-sprite frame batch. The
        // pool caps at 512 shots, and a held trigger on the fastest catalog loadout sustains
        // ~76 in flight, so ~150 sprites in practice. The plume is affordable precisely because
        // it is exclusive to the two slowest-firing weapons in the catalog -- a 4 s and a 9 s
        // reload put at most a handful of missiles in the air at once.
        //
        // custom.x stays 0 here, deliberately: on a radially symmetric orb the heat-distortion
        // warp and the tail->head temperature ramp both key off UV.y and would shear it. The
        // glow comes from custom.w instead, which feeds the same shader term without the
        // directional effects -- the same channel split draw_glow_line uses and for the same
        // reason.
        bs_sprite head{};
        head.position = draw_pos;
        head.size     = Vec2{ p.radius * head_mul, p.radius * head_mul };
        head.origin   = Vec2{ 0.5f, 0.5f };
        head.rotation = 0.0f;
        head.uv       = bs_rect{ 0.0f, 0.0f, 1.0f, 1.0f };
        // Bias the shot's own colour hard toward white so the head reads as incandescent while
        // the streak behind it keeps the tint that makes friend and foe distinguishable.
        head.tint     = bs_color{ p.color.r + (1.0f - p.color.r) * 0.65f,
                                  p.color.g + (1.0f - p.color.g) * 0.65f,
                                  p.color.b + (1.0f - p.color.b) * 0.65f,
                                  0.9f };
        head.custom   = bs_color{ 0.0f, 0.0f, 1.0f, 0.75f };
        head.texture       = flash_texture;
        head.blend         = BLEND_ADDITIVE;
        head.layer           = layer;
        head.glow_override = glow_override;
        renderer_draw_sprite(&head);
        // ---- Engine plume (VFX_ORDNANCE only) ------------------------------------------------
        // What separates a missile from a shell is that it is UNDER POWER the whole way, and the
        // reference doc's section 6.4 says to show that with a small engine flame and hot
        // exhaust -- explicitly NOT with a conventional smoke trail, which does not belong in
        // vacuum (section 12). So this is a second, shorter, much hotter streak laid over the
        // dim one above: bright at the nozzle, tapering aft, and never longer than the body's
        // own trail.
        //
        // The pulse is section 7's "animate the energy state, not just the position". It is
        // deliberately shallow -- +/-18% around 0.82 -- because that section also warns the goal
        // is controlled instability, not flicker. Driving it off `age` rather than a frame
        // counter keeps two missiles launched a moment apart out of phase for free.
        if (p.vfx_family == 2) {
            f32 pulse = 0.82f + 0.18f * sinf(p.age * 26.0f);
            bs_sprite plume{};
            plume.position = draw_pos;
            plume.size     = Vec2{ p.radius * 2.6f, p.radius * 9.0f * pulse };
            plume.origin   = Vec2{ 0.5f, 1.0f };
            plume.rotation = streak.rotation;   // same aft-facing axis as the trail
            plume.uv       = bs_rect{ 0.0f, 0.0f, 1.0f, 1.0f };
            // Exhaust runs hot white at the nozzle into the round's own tint; the temperature
            // ramp is left to the shader via custom.x, exactly as the streak does it, because a
            // plume is one of the few places that white-hot-head/red-tail gradient is literally
            // correct rather than merely decorative.
            plume.tint     = bs_color{ 1.0f, 0.93f, 0.80f, 0.85f * pulse };
            plume.custom   = bs_color{ 1.0f, p.age, 1.0f, 0.0f };
            plume.texture       = streak_texture;
            plume.blend         = BLEND_ADDITIVE;
            plume.layer         = layer;
            plume.glow_override = glow_override;
            renderer_draw_sprite(&plume);
        }
    }
}
