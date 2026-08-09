#include "render/projectile_fx.h"

#include "game.h"                  // game_state (projectile_fx ring, render.bullet_glow)
#include "core/view_transform.h"   // render_from_hierpos
#include "core/render_layers.h"    // LAYER_PROJECTILE_FX
#include "sim/weapon_def.h"        // VfxFamily (explicit peer include, not via the game.h cascade)

#include <renderer/renderer.h>
#include <math.h>

using namespace bs_math;

// =====================================================================================
// Procedural textures.
//
// Baked into a SHARED FILE-STATIC buffer, not on the stack. 128*128*4 is 64 KB, and three of
// them in a row would be 192 KB of stack in one frame -- the same trap render/text.cpp and
// render/star_fx.cpp already dodge with statics, and the one sim/projectile.cpp still falls
// into. Reusing one buffer across the three bakes keeps the resident cost at 64 KB, and it is
// only touched during init.
// =====================================================================================

static bs_texture g_tex_flare; // hot core + soft halo; the body of every flash
static bs_texture g_tex_ring;  // thin annulus; shock fronts and the flak airburst
static bs_texture g_tex_spark; // tapered wedge; the muzzle gas cone and impact debris

#define FX_TEX_DIM 128

static u8 g_bake[FX_TEX_DIM * FX_TEX_DIM * 4];

// White RGB throughout: every effect is additive and takes its colour from the sprite tint,
// so baking colour into the texture would only fight the per-shot tint that keeps friend and
// foe readable.
static void bake_pixel(i32 x, i32 y, i32 w, f32 alpha) {
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    i32 idx = (y * w + x) * 4;
    g_bake[idx + 0] = 255;
    g_bake[idx + 1] = 255;
    g_bake[idx + 2] = 255;
    g_bake[idx + 3] = (u8)(alpha * 255.0f);
}

void projectile_fx_render_init() {
    const f32 c = (f32)FX_TEX_DIM * 0.5f;

    // ---- Flare: a tight incandescent core inside a wide halo ---------------------------
    // Two lobes rather than one Gaussian. A single falloff either has a hard core and no
    // spill, or spill and no core; a hit needs both -- the core is what the bloom threshold
    // catches, the halo is what makes it feel like it lit the surrounding vacuum.
    {
        const f32 core_sigma = 7.0f;
        const f32 halo_sigma = 27.0f;
        for (i32 y = 0; y < FX_TEX_DIM; ++y) {
            for (i32 x = 0; x < FX_TEX_DIM; ++x) {
                f32 dx = (f32)x - c, dy = (f32)y - c;
                f32 d2 = dx * dx + dy * dy;
                f32 core = expf(-d2 / (2.0f * core_sigma * core_sigma));
                f32 halo = expf(-d2 / (2.0f * halo_sigma * halo_sigma));
                bake_pixel(x, y, FX_TEX_DIM, core + halo * 0.45f);
            }
        }
        g_tex_flare = renderer_create_texture(g_bake, FX_TEX_DIM, FX_TEX_DIM);
    }

    // ---- Ring: a soft-edged annulus ----------------------------------------------------
    // Peak at 0.78 of the half-width, not at 1.0, so the falloff has room to reach ~0 before
    // the texture edge. A ring that is still bright at the border draws its own square.
    {
        const f32 r0 = 0.78f;
        const f32 w  = 0.10f;
        for (i32 y = 0; y < FX_TEX_DIM; ++y) {
            for (i32 x = 0; x < FX_TEX_DIM; ++x) {
                f32 dx = ((f32)x - c) / c, dy = ((f32)y - c) / c;
                f32 r = sqrtf(dx * dx + dy * dy);
                f32 t = (r - r0) / w;
                bake_pixel(x, y, FX_TEX_DIM, expf(-t * t));
            }
        }
        g_tex_ring = renderer_create_texture(g_bake, FX_TEX_DIM, FX_TEX_DIM);
    }

    // ---- Spark: a wedge, wide and bright at v=1, tapering to nothing at v=0 -------------
    // Matches sim/projectile.cpp's streak convention (v=1 is the anchored end) so the same
    // origin/rotation maths applies to both. 32x128 -- it is only ever stretched along its
    // length, so width resolution buys nothing.
    {
        const i32 SW = 32, SH = 128;
        const f32 sc = (f32)SW * 0.5f;
        for (i32 y = 0; y < SH; ++y) {
            f32 v = (f32)y / (f32)(SH - 1);            // 0 = tip, 1 = anchored base
            f32 len   = powf(v, 1.6f);                  // bright at the base, fades to the tip
            f32 sigma = 1.6f + 5.4f * v;                // and narrows toward the tip
            for (i32 x = 0; x < SW; ++x) {
                f32 dx = (f32)x - sc;
                bake_pixel(x, y, SW, len * expf(-(dx * dx) / (2.0f * sigma * sigma)));
            }
        }
        g_tex_spark = renderer_create_texture(g_bake, SW, SH);
    }
}

// =====================================================================================
// Draw helpers
// =====================================================================================

static f32 ease_out(f32 t) { f32 k = 1.0f - t; return 1.0f - k * k * k; }

// The ring texture's bright band peaks at 0.78 of the quad's HALF-width, so a quad of side S
// draws its ring at world radius 0.39*S. Every ring below is authored as the radius the player
// should see and converted through this -- otherwise "how big is that ring" is a question you
// have to answer by reading the texture bake.
#define RING_QUAD(radius) ((radius) * 2.564f)

// Orient a spark so it extends FORWARD along `d` from its anchor. The spark and streak
// textures are baked bright-end-at-v=1 and drawn with origin {0.5, 1.0}, which puts the quad
// BEHIND the pivot -- correct for a projectile trail, backwards for a muzzle cone or a debris
// spoke. Negating the direction flips it without a second texture.
static f32 forward_rotation(Vec2 d) { return atan2f(-d.y, -d.x) + BS_PI * 0.5f; }

// One additive, self-emissive sprite. custom.z = 1 keeps it out of the scene-lighting branch
// (mandatory on LAYER_PROJECTILE_FX, which sits below frame_lighting's unlit cutoff), and the
// glow rides on custom.w rather than custom.x so it picks up the radial glow term WITHOUT the
// heat-distortion warp and tail->head temperature ramp that custom.x also switches on -- the
// same channel split, and the same reason, as draw_glow_line.
static void submit(const bs_glow_params* glow, bs_texture tex, Vec2 pos, Vec2 size,
                   Vec2 origin, f32 rotation, bs_color tint, f32 glow_amt) {
    if (tint.a <= 0.002f) return;   // fully faded: skip the batch slot entirely
    bs_sprite sp{};
    sp.position      = pos;
    sp.size          = size;
    sp.origin        = origin;
    sp.rotation      = rotation;
    sp.uv            = bs_rect{ 0.0f, 0.0f, 1.0f, 1.0f };
    sp.tint          = tint;
    sp.custom        = bs_color{ 0.0f, 0.0f, 1.0f, glow_amt };
    sp.texture       = tex;
    sp.blend         = BLEND_ADDITIVE;
    sp.layer         = LAYER_PROJECTILE_FX;
    sp.glow_override = glow;
    renderer_draw_sprite(&sp);
}

// Push a colour toward white by `k`. Every effect has an incandescent centre and a tinted
// edge; this is how the shot's own colour survives at the rim while the core reads as heat.
static bs_color hot(bs_color c, f32 k, f32 alpha) {
    return bs_color{ c.r + (1.0f - c.r) * k,
                     c.g + (1.0f - c.g) * k,
                     c.b + (1.0f - c.b) * k,
                     alpha };
}

// =====================================================================================
// Per-kind geometry.
//
// Grounded-kinetic throughout: short lifetimes, fast attack, no lingering fireballs, and
// every dimension derived from the shot's own stats -- `scale` from proj_radius (or the flak
// burst radius) and `power` from damage. That is what makes this code-driven rather than
// authored: a new .weapon file gets effects sized to its shell with no VFX work, exactly the
// way mount_art and muzzle offsets stay optional data on top of a working default.
//
// Sizes are WORLD units, not screen pixels, so an effect shrinks as the player zooms out
// instead of blooming into a screen-sized blob at galaxy scale.
//
// SIZING REFERENCE -- read this before changing a multiplier. `scale` is proj_radius, which
// runs 3.0 (autocannon) to 8.0 (torpedo). Those numbers are TINY against everything else in
// the scene, and that is the trap: the hull is 1586 units long, a medium turret's art is
// ~250 units across, and a flak burst is 240 units in radius. An effect drawn at 5x
// proj_radius is 20 units -- under two percent of the ship it just left, which at combat
// framing is about four pixels and reads as a rendering artifact rather than as a gun going
// off. The multipliers below are therefore in the 12x-45x band, putting a medium cannon's
// muzzle flash around 80-130 units (a third to half a flak burst) and its impact around 130
// core / 175 ring. That is the scale at which the events read as events.
// =====================================================================================

static void draw_muzzle(const bs_glow_params* glow, const ProjectileFxEvent& e, Vec2 p) {
    const f32 t = e.age / e.life;
    const f32 f = 1.0f - t;
    const f32 s = e.scale;

    // What leaves the mount differs by family because the PHYSICS of the launch differs, and
    // that is the cheapest honest way to make three weapons read apart:
    //
    //   SHELL     burning propellant -- a hot directional gas cone plus a faint blast front.
    //   SLUG      an electromagnetic snap. There is no expanding gas at all, so the ring nearly
    //             vanishes and the cone becomes long and needle-thin. This is the single most
    //             recognisable cue that a railgun is not a cannon, and it costs one branch.
    //   ORDNANCE  rocket ignition -- short, broad and soft rather than a directional jet, with
    //             the widest blast front of the three because a motor really does dump gas.
    f32 cone_len_a, cone_len_b, cone_w_a, cone_w_b;
    f32 core_a, core_b, core_white;
    f32 ring_a, ring_b, ring_alpha;
    switch (e.family) {
        case VFX_SLUG:
            cone_len_a = 34.0f; cone_len_b = 12.0f; cone_w_a = 3.5f;  cone_w_b = 1.5f;
            core_a     = 11.0f; core_b     =  5.0f; core_white = 0.92f;
            ring_a     =  2.0f; ring_b     =  3.0f; ring_alpha = 0.08f;
            break;
        case VFX_ORDNANCE:
            cone_len_a = 14.0f; cone_len_b =  6.0f; cone_w_a = 11.0f; cone_w_b = 4.0f;
            core_a     = 20.0f; core_b     =  8.0f; core_white = 0.62f;
            ring_a     =  5.0f; ring_b     =  9.0f; ring_alpha = 0.40f;
            break;
        default: // VFX_SHELL
            cone_len_a = 22.0f; cone_len_b = 10.0f; cone_w_a = 7.0f;  cone_w_b = 3.0f;
            core_a     = 16.0f; core_b     =  7.0f; core_white = 0.80f;
            ring_a     =  3.0f; ring_b     =  6.0f; ring_alpha = 0.28f;
            break;
    }

    // Gas cone out the barrel. Drawn first so the core sits on top of its base.
    f32 cone_len = s * (cone_len_a + cone_len_b * e.power) * (0.45f + 0.55f * f);
    f32 cone_w   = s * (cone_w_a + cone_w_b * e.power);
    submit(glow, g_tex_spark, p, Vec2{ cone_w, cone_len }, Vec2{ 0.5f, 1.0f },
           forward_rotation(e.dir), hot(e.tint, 0.55f, f * f), 0.7f);

    // Incandescent core at the muzzle itself.
    f32 core = s * (core_a + core_b * e.power) * (0.65f + 0.35f * f);
    submit(glow, g_tex_flare, p, Vec2{ core, core }, Vec2{ 0.5f, 0.5f }, 0.0f,
           hot(e.tint, core_white, f * f), 0.9f);

    // Blast front. Kept faint and TIGHT -- it is the difference between a gun and a firework,
    // and at first pass it was neither: a wide bright ring outran the core and the muzzle read
    // as a smoke puff with a hoop around it rather than as a barrel discharging. The core and
    // the cone carry this effect; the ring only has to suggest gas expanding.
    f32 ring = RING_QUAD(s * (ring_a + ring_b * e.power) * ease_out(t));
    submit(glow, g_tex_ring, p, Vec2{ ring, ring }, Vec2{ 0.5f, 0.5f }, 0.0f,
           hot(e.tint, 0.35f, f * f * ring_alpha), 0.3f);
}

static void draw_impact(const bs_glow_params* glow, const ProjectileFxEvent& e, Vec2 p, i32 slot) {
    const f32 t  = e.age / e.life;
    const f32 f  = 1.0f - t;
    const f32 f2 = f * f;
    const f32 s  = e.scale;

    // How a round terminates is the other half of its identity:
    //
    //   SHELL     punches through -- a broad back-spray of debris and a clean flash.
    //   SLUG      the reference doc's section 6.3 asks specifically for a STRONG DIRECTIONAL
    //             impact, so the spray narrows to a tight cone and the spokes get long: all the
    //             energy went one way and the debris should say so. Hottest, smallest core.
    //   ORDNANCE  section 6.4 asks for a physical explosion -- the spray goes nearly
    //             omnidirectional, the spokes shorten and fatten into chunks, and the core and
    //             ring both grow. Warmest tint of the three; a detonation is not white-hot.
    f32 spread, spoke_len_a, spoke_len_b, spoke_wid_a, spoke_wid_b;
    f32 ring_a, ring_b, core_a, core_b, core_white;
    switch (e.family) {
        case VFX_SLUG:
            spread = 0.85f; spoke_len_a = 26.0f; spoke_len_b = 20.0f;
            spoke_wid_a = 1.6f; spoke_wid_b = 1.0f;
            ring_a =  8.0f; ring_b = 44.0f; core_a =  9.0f; core_b = 16.0f; core_white = 0.95f;
            break;
        case VFX_ORDNANCE:
            spread = 2.60f; spoke_len_a = 12.0f; spoke_len_b = 12.0f;
            spoke_wid_a = 4.0f; spoke_wid_b = 2.0f;
            ring_a = 10.0f; ring_b = 46.0f; core_a = 18.0f; core_b = 26.0f; core_white = 0.60f;
            break;
        default: // VFX_SHELL
            spread = 1.75f; spoke_len_a = 16.0f; spoke_len_b = 16.0f;
            spoke_wid_a = 2.5f; spoke_wid_b = 1.5f;
            ring_a =  6.0f; ring_b = 38.0f; core_a = 12.0f; core_b = 20.0f; core_white = 0.85f;
            break;
    }

    // Debris, thrown back along the incoming shot. Four spokes fanned across the family's own
    // cone, with a per-event angular offset so two hits on the same hull do not stamp the
    // identical star. The offset is derived from the ring slot rather than a random draw --
    // free, and it keeps the whole pass deterministic (Math.random has no place in a frame
    // that may be replayed by the profiler).
    const f32 base   = atan2f(-e.dir.y, -e.dir.x);
    const f32 jitter = (f32)(((u32)slot * 2654435761u) % 1000u) * 0.001f;
    for (i32 k = 0; k < 4; ++k) {
        f32 a = base + ((f32)k + jitter - 1.5f) * (spread / 3.0f);
        Vec2 d = Vec2{ cosf(a), sinf(a) };
        f32 len = s * (spoke_len_a + spoke_len_b * e.power) * (0.35f + 0.65f * ease_out(t));
        f32 wid = s * (spoke_wid_a + spoke_wid_b * e.power) * f;
        submit(glow, g_tex_spark, p, Vec2{ wid, len }, Vec2{ 0.5f, 1.0f },
               forward_rotation(d), hot(e.tint, 0.45f, f2 * 0.85f), 0.5f);
    }

    // Shock front.
    f32 ring = RING_QUAD(s * (ring_a + ring_b * e.power) * ease_out(t));
    submit(glow, g_tex_ring, p, Vec2{ ring, ring }, Vec2{ 0.5f, 0.5f }, 0.0f,
           hot(e.tint, 0.40f, f * f * sqrtf(f) * 0.7f), 0.35f);

    // Core flash. Decays fastest of the three so the hit has a hard front edge -- the flash is
    // gone while the ring and debris are still travelling, which is what reads as an impact
    // rather than an explosion.
    f32 core = s * (core_a + core_b * e.power) * (1.0f - 0.35f * t);
    submit(glow, g_tex_flare, p, Vec2{ core, core }, Vec2{ 0.5f, 0.5f }, 0.0f,
           hot(e.tint, core_white, f2 * f), 1.0f);
}

static void draw_burst(const bs_glow_params* glow, const ProjectileFxEvent& e, Vec2 p) {
    const f32 t = e.age / e.life;
    const f32 f = 1.0f - t;
    const f32 s = e.scale;   // the FLAK BURST RADIUS, not the shell radius

    // The outer ring expands to exactly the radius that did damage, so the effect doubles as
    // the readout for how far the screen reached. Flak is the one weapon whose area matters to
    // the player, and this is the only place the game ever shows it.
    // Unlike every other effect here, `scale` is already a real world distance, so this one
    // needs no size multiplier at all -- the ring is drawn at exactly `scale` and nothing else.
    f32 ring = RING_QUAD(s * ease_out(t));
    submit(glow, g_tex_ring, p, Vec2{ ring, ring }, Vec2{ 0.5f, 0.5f }, 0.0f,
           hot(e.tint, 0.30f, f * sqrtf(f) * 0.8f), 0.4f);

    // A faster inner pop for the fuse, at ~2.2x the outer ring's rate.
    f32 ft = (t * 2.2f > 1.0f) ? 1.0f : t * 2.2f;
    f32 fuse = RING_QUAD(s * 0.45f * ease_out(ft));
    submit(glow, g_tex_ring, p, Vec2{ fuse, fuse }, Vec2{ 0.5f, 0.5f }, 0.0f,
           hot(e.tint, 0.60f, (1.0f - ft) * (1.0f - ft)), 0.6f);

    // Detonation core.
    f32 core = s * 0.55f * (0.25f + 0.75f * ease_out(t));
    submit(glow, g_tex_flare, p, Vec2{ core, core }, Vec2{ 0.5f, 0.5f }, 0.0f,
           hot(e.tint, 0.75f, f * f * 0.9f), 0.8f);
}

static void draw_intercept(const bs_glow_params* glow, const ProjectileFxEvent& e, Vec2 p) {
    const f32 t = e.age / e.life;
    const f32 f = 1.0f - t;
    const f32 s = e.scale;

    // White-cyan rather than the round's own colour, matching the point-defense beam in
    // render/defense_laser_overlay.cpp. The kill should read as the DEFENDER's work, so it is
    // tinted by what killed it, not by what was killed.
    const bs_color PD = bs_color{ 0.80f, 0.98f, 1.00f, 1.0f };

    f32 ring = RING_QUAD(s * (3.0f + 6.0f * e.power) * ease_out(t));
    submit(glow, g_tex_ring, p, Vec2{ ring, ring }, Vec2{ 0.5f, 0.5f }, 0.0f,
           bs_color{ PD.r, PD.g, PD.b, f * f * 0.6f }, 0.4f);

    f32 core = s * (9.0f + 5.0f * e.power) * (1.0f - 0.4f * t);
    submit(glow, g_tex_flare, p, Vec2{ core, core }, Vec2{ 0.5f, 0.5f }, 0.0f,
           bs_color{ PD.r, PD.g, PD.b, f * f }, 0.9f);
}

// =====================================================================================

// =====================================================================================
// Muzzle charge-up.
//
// Section 28 of the VFX reference gives the sequence charge -> muzzle flash -> projectile ->
// trail -> impact, and notes that for HEAVY weapons a short charge-up greatly improves
// perceived power. The other four stages already exist; this is the missing first one.
//
// Two rules keep it honest:
//
//   1. It reads and never writes. `cooldown_progress()` is computed by the simulation for its
//      own reasons and already consumed by the weapon hub. A charge-up that delayed the shot to
//      make room for itself would be a fire-rate change wearing a VFX costume.
//   2. It is gated on CYCLE TIME, not on a weapon list. Anything that cycles faster than
//      CHARGE_MIN_CYCLE never charges, because a build-up shorter than the gap between shots
//      reads as a barrel that is simply always lit. That gate falls out of authored stats and
//      needs no new data: at 12/s the autocannon is 0.083 s per shot and 5/s the gauss 0.2 s,
//      both far under; trident (1.67 s), longlance (1.25 s), harpoon (4 s) and torpedo (9 s)
//      all clear it. A new weapon sorts itself.
// =====================================================================================

#define CHARGE_MIN_CYCLE 1.0f   // seconds; below this a weapon never charges
#define CHARGE_WINDOW    0.4f   // seconds of build-up before ready

// Deliberately ABSOLUTE, not a fraction of the cycle. The same 0.4 s of anticipation reads
// correctly on a 1.25 s railgun and a 9 s torpedo; the equivalent fraction would differ between
// them by a factor of seven and the torpedo would glow for most of a reload.

static void draw_charge_at(const bs_glow_params* glow, Vec2 p, f32 t01, f32 unit,
                           u8 family, f32 phase) {
    // t01 ramps 0 -> 1 across the window. Alpha is squared so the first half stays almost
    // invisible and the payoff crowds into the last moments -- section 27's point that timing
    // matters more than detail. Size grows far less than brightness does, so the effect reads
    // as heat building rather than as something inflating.
    // SIZING: `unit` is the hardpoint half-extent (60 world units on a MEDIUM slot), and these
    // multipliers are deliberately small against it. The anticipation must stay SMALLER than the
    // payoff it sets up -- the first pass used 5.6x here, which put a 336-unit charge halo in
    // front of a 139-unit muzzle flash, so the gun's brightest moment was the wind-up rather
    // than the shot. Section 26's brightness hierarchy applies across time, not just within one
    // effect. Keep the peak charge at roughly half the flash it hands off to.
    const f32 grow = 0.45f + 0.55f * t01;
    f32 core_mul, halo_mul, alpha;
    bs_color tint;
    switch (family) {
        case VFX_SLUG:
            // Rail capacitors: cold, electric, and the only family that visibly flickers. This
            // is the charge the archetype most obviously wants -- there is no propellant to
            // burn, so the build-up IS the weapon's tell, and it earns being the largest of
            // the three by a small margin.
            tint = bs_color{ 0.62f, 0.84f, 1.00f, 1.0f };
            core_mul = 1.15f; halo_mul = 2.7f;
            alpha = t01 * t01 * (0.88f + 0.12f * sinf(phase * 47.0f));
            break;
        case VFX_ORDNANCE:
            // A launcher does not charge, it ARMS. Slow, dim and warm: a tube cycling, not a
            // capacitor filling, so it stays well under the ballistic families.
            tint = bs_color{ 1.00f, 0.62f, 0.34f, 1.0f };
            core_mul = 0.95f; halo_mul = 2.1f;
            alpha = t01 * t01 * 0.55f;
            break;
        default: // VFX_SHELL
            tint = bs_color{ 1.00f, 0.78f, 0.42f, 1.0f };
            core_mul = 1.00f; halo_mul = 2.3f;
            alpha = t01 * t01 * 0.75f;
            break;
    }
    f32 halo = unit * halo_mul * grow;
    submit(glow, g_tex_flare, p, Vec2{ halo, halo }, Vec2{ 0.5f, 0.5f }, 0.0f,
           bs_color{ tint.r, tint.g, tint.b, alpha * 0.45f }, 0.5f);
    f32 core = unit * core_mul * grow;
    submit(glow, g_tex_flare, p, Vec2{ core, core }, Vec2{ 0.5f, 0.5f }, 0.0f,
           hot(tint, 0.55f, alpha), 0.85f);
}

// One ship's mounts. Split out because the fleet, the enemy hull and any future combatant all
// want identical treatment and none of them share an iteration path.
static void draw_ship_charges(const game_state* s, const bs_glow_params* glow, const Ship& sh) {
    for (i32 hp = 0; hp < sh.hardpoint_count; ++hp) {
        const Weapon* w = sh.mounts[hp];
        if (!w || w->disabled) continue;

        const f32 cycle = w->cooldown_duration_s();
        if (cycle < CHARGE_MIN_CYCLE) continue;          // too fast to read as a build-up

        // Absolute seconds to ready, recovered from the fraction the simulation exposes.
        const f32 remaining = (1.0f - w->cooldown_progress()) * cycle;
        if (remaining <= 0.0f || remaining > CHARGE_WINDOW) continue;

        // Drawn ONLY while still cooling, never while loaded and idle. Under a held trigger the
        // weapon fires the instant it is ready, so the build hands straight off to the muzzle
        // flash; at rest it shows nothing. "This gun is loaded" is a readiness readout, and the
        // weapon hub already owns that from the same cooldown_progress() call -- repeating it
        // in-world would be a second answer to a question that already has one.
        const f32 t01 = 1.0f - (remaining / CHARGE_WINDOW);

        const f32 unit = hardpoint_half_extent(sh.hardpoints[hp].size) * sh.world_scale;
        const WeaponDef* def = w->def;
        const u8  family = def ? def->vfx_family : (u8)VFX_SHELL;
        const i32 n      = def ? def->muzzle_count : 0;

        if (n <= 0) {
            HierPos2 c = ship_muzzle_origin(&sh, hp, -1);   // -1 => the mount centre
            draw_charge_at(glow, render_from_hierpos(s, &c), t01, unit, family, remaining);
            continue;
        }
        // Which barrel lights depends on what the trigger will actually do: a SALVO empties all
        // of them, so all of them charge; a SEQUENTIAL gun fires one, so only the barrel that is
        // next in rotation charges. That makes the charge-up a genuine readout of the weapon's
        // firing pattern rather than decoration, and it comes free from data already authored.
        const b8  salvo = (def->muzzle_pattern == MUZZLE_SALVO);
        const i32 first = salvo ? 0 : (i32)(w->next_muzzle % n);
        const i32 count = salvo ? n : 1;
        for (i32 k = 0; k < count; ++k) {
            HierPos2 o = ship_muzzle_origin(&sh, hp, (first + k) % n);
            draw_charge_at(glow, render_from_hierpos(s, &o), t01, unit, family, remaining);
        }
    }
}

void projectile_fx_draw_charges(const game_state* s) {
    if (!s) return;
    const bs_glow_params* glow = &s->render.bullet_glow;
    const Fleet& fleet = s->fleet_state.fleet;
    for (i32 i = 0; i < fleet.count(); ++i)
        draw_ship_charges(s, glow, fleet.at(i).ship);
    // The enemy hull too -- a heavy gun spinning up is exactly the kind of thing that should
    // telegraph. NPC agents are deliberately skipped: they fire through Weapon::fire from their
    // hull origin rather than through a hardpoint, and draw no mount art, so there is no barrel
    // for a charge to sit on (see ShipCombatModel's note on that fire site).
    draw_ship_charges(s, glow, s->fleet_state.enemy_ship);
}

void projectile_fx_draw(const game_state* s) {
    if (!s) return;
    const ProjectileFx& ring = s->projectile_fx;
    if (ring.live <= 0) return;   // nothing live: the whole pass costs one branch

    // Share the projectile pass's glow block. It is long-lived storage inside game_state,
    // which the boundary doc requires -- renderer_draw_sprite copies this POINTER into the
    // frame batch and the backend dereferences it during end_frame, so a stack-local would
    // dangle. Sharing it with the streaks also means one glow identity across both layers
    // instead of two, which is what the backend breaks draw runs on.
    const bs_glow_params* glow = &s->render.bullet_glow;

    for (i32 i = 0; i < MAX_PROJECTILE_FX; ++i) {
        const ProjectileFxEvent& e = ring.events[i];
        if (!e.active) continue;

        Vec2 p = render_from_hierpos(s, &e.position);

        switch (e.kind) {
            case PFX_MUZZLE:    draw_muzzle(glow, e, p);       break;
            case PFX_IMPACT:    draw_impact(glow, e, p, i);    break;
            case PFX_BURST:     draw_burst(glow, e, p);        break;
            case PFX_INTERCEPT: draw_intercept(glow, e, p);    break;
            default: break;
        }
    }
}
