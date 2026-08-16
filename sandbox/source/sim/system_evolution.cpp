#include "sim/system_evolution.h"
#include "sim/galaxy_rng.h"   // galaxy_splitmix64 / GalaxyRng (per-body/per-epoch streams)
#include "sim/ss_generation.h"    // worldgen_orbit_range_au / worldgen_star / planet_type_name
#include <core/logger.h>
#include <math.h>
#include <string.h>           // memcmp (self-test determinism check)

// =====================================================================================
// Epoch-based planetary evolution (see header for the phase overview).
// =====================================================================================
// Everything is fixed-size stack data; RNG streams are keyed (seed, core slot, epoch) so a
// conditional draw for one body never perturbs another. Epoch numbering is continuous
// across phases for the chronicle: 0 = disk, 1..8 = accretion, 9..14 = geophys, 15 = today.

#define EVO_MAX_CORES        10   // protoplanet cores seeded by the disk (merged/ejected down)
#define EVO_ACCRETION_EPOCHS 8
#define EVO_GEO_EPOCHS       6
#define EARTHS_PER_SOLAR     333000.0f

// ------------------------------------------------------------------ RNG streams --------

static inline GalaxyRng evo_rng(u64 seed, i32 body, i32 epoch) {
    u64 s = seed ^ (0xB5297A4D3F84C2E1ull * (u64)(body + 1))
                 ^ (0x68E31DA4B4B2C8D5ull * (u64)(epoch + 1));
    return galaxy_rng_seed(galaxy_splitmix64(s));
}

static inline f32 clamp01(f32 v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
static inline f32 clampf(f32 v, f32 lo, f32 hi) { return v < lo ? lo : (v > hi ? hi : v); }

static BodyComposition comp_normalize(BodyComposition c) {
    f32 s = c.metal + c.silicate + c.ice + c.gas;
    if (s <= 1e-6f) return BodyComposition{ 0.0f, 1.0f, 0.0f, 0.0f };
    f32 inv = 1.0f / s;
    return BodyComposition{ c.metal * inv, c.silicate * inv, c.ice * inv, c.gas * inv };
}

// Mass-weighted mix of two compositions (mergers, gas envelope accretion).
static BodyComposition comp_mix(BodyComposition a, f32 ma, BodyComposition b, f32 mb) {
    f32 s = ma + mb;
    if (s <= 1e-6f) return a;
    f32 wa = ma / s, wb = mb / s;
    BodyComposition r;
    r.metal    = a.metal    * wa + b.metal    * wb;
    r.silicate = a.silicate * wa + b.silicate * wb;
    r.ice      = a.ice      * wa + b.ice      * wb;
    r.gas      = a.gas      * wa + b.gas      * wb;
    return comp_normalize(r);
}

// Hill radius (AU) of a body of `m_earth` orbiting a `m_star_solar` star at `a` AU.
static inline f32 hill_radius_au(f32 a, f32 m_earth, f32 m_star_solar) {
    return a * cbrtf(m_earth / (3.0f * m_star_solar * EARTHS_PER_SOLAR));
}

static void evo_log(EvolvedSystem* s, u8 epoch, u8 kind, i8 body, i8 other) {
    if (s->event_count >= EVO_MAX_EVENTS) return;
    s->events[s->event_count++] = EvolutionEvent{ epoch, kind, body, other };
}

// ------------------------------------------------------------------ protoplanets -------

// Working record for one protoplanet core during phases 1-2. Cores keep their slot index
// for the whole run (RNG stream identity); `alive`/`is_belt` track their fate.
struct ProtoCore {
    f32 a;             // semi-major axis (AU)
    f32 e;
    f32 mass;          // Mearth (solids + gas envelope)
    f32 feed;          // remaining solid feedstock in this core's annulus (Mearth)
    BodyComposition comp;
    b8  alive;
    b8  giant;         // underwent runaway gas accretion
    b8  is_belt;       // ground down into an asteroid belt
    b8  migrated;      // logged a migration event already
    b8  impact_moon;   // a late giant impact spun off a moon
    f32 belt_width;    // belts: annulus half-width (AU)
};

// =====================================================================================
// Phase 1 — disk condensation. Build the 1-D disk profile from the star and seed
// Hill-spaced protoplanet cores, each owning its annulus' solid feedstock budget.
// =====================================================================================
static i32 phase1_disk(ProtoCore* cores, const StarProperties& star, u64 seed) {
    f32 au_min, au_max;
    worldgen_orbit_range_au(star, &au_min, &au_max);

    GalaxyRng disk = evo_rng(seed, /*body*/ -1, /*epoch*/ 0);
    i32 n = galaxy_rng_int(&disk, 5, EVO_MAX_CORES);
    // Disk mass scales with stellar mass and metallicity; sets the solid budget scale.
    f32 solid_scale = galaxy_rng_range(&disk, 3.5f, 8.0f) * star.metallicity * star.mass_solar;

    f32 log_span = logf(au_max / au_min);
    for (i32 i = 0; i < n; ++i) {
        GalaxyRng r = evo_rng(seed, i, 0);
        ProtoCore* c = &cores[i];
        // Log-spaced annulus centre with jitter (keeps inner packing tight, outer sparse).
        f32 t = ((f32)i + galaxy_rng_range(&r, 0.15f, 0.85f)) / (f32)n;
        c->a = au_min * expf(t * log_span);
        c->e = galaxy_rng_range(&r, 0.0f, 0.06f);
        c->mass = galaxy_rng_range(&r, 0.02f, 0.12f); // lunar-to-Mars scale embryo

        // Solid surface density ~ a^-1.5 x metallicity; annulus width ~ a (log spacing), so
        // the annulus solid budget falls off as ~a^-0.5. Ice condensing past the frost line
        // multiplies the available solids.
        b8 icy = c->a > star.frost_line_au;
        f32 budget = solid_scale / sqrtf(c->a / au_min);
        if (icy) budget *= 4.0f;
        c->feed = budget * galaxy_rng_range(&r, 0.6f, 1.4f);

        if (icy) {
            c->comp = BodyComposition{ 0.10f, 0.32f, 0.58f, 0.0f };
        } else {
            f32 metal = clampf(0.30f + 0.12f * (star.metallicity - 1.0f), 0.18f, 0.45f);
            c->comp = BodyComposition{ metal, 1.0f - metal, 0.0f, 0.0f };
        }
        c->alive = TRUE;
        c->giant = FALSE; c->is_belt = FALSE; c->migrated = FALSE; c->impact_moon = FALSE;
        c->belt_width = 0.0f;
    }
    return n;
}

// =====================================================================================
// Phase 2 — accretion & migration. Fixed epoch loop: cores eat their feedstock, big cold
// cores go runaway before the gas disk disperses, giants migrate inward, Hill-unstable
// pairs merge or eject, frustrated annuli grind into belts.
// =====================================================================================
static void phase2_accretion(ProtoCore* cores, i32 n, const StarProperties& star,
                             u64 seed, EvolvedSystem* out) {
    GalaxyRng setup = evo_rng(seed, -1, 1);
    // Gas disk lifetime: hot massive stars photo-evaporate their disks fast.
    i32 dispersal_epoch = galaxy_rng_int(&setup, 4, 6);
    if (star.mass_solar > 1.5f) dispersal_epoch -= 2;
    f32 au_min, au_max;
    worldgen_orbit_range_au(star, &au_min, &au_max);

    i32 order[EVO_MAX_CORES]; // alive cores sorted by a, rebuilt each epoch

    for (i32 epoch = 1; epoch <= EVO_ACCRETION_EPOCHS; ++epoch) {
        if (epoch == dispersal_epoch)
            evo_log(out, (u8)epoch, EVO_EV_DISK_DISPERSED, -1, -1);

        // --- growth + runaway + migration (per-core streams) ---
        for (i32 i = 0; i < n; ++i) {
            ProtoCore* c = &cores[i];
            if (!c->alive || c->is_belt) continue;
            GalaxyRng r = evo_rng(seed, i, epoch);

            f32 dm = c->feed * galaxy_rng_range(&r, 0.35f, 0.55f);
            c->feed -= dm;
            c->mass += dm;

            // Runaway gas accretion: a massive-enough core while gas remains.
            if (!c->giant && epoch < dispersal_epoch && c->mass >= 6.0f) {
                f32 gas = c->mass * galaxy_rng_range(&r, 1.5f, 6.0f)
                        * (f32)(dispersal_epoch - epoch); // earlier runaway -> fatter giant
                c->comp = comp_mix(c->comp, c->mass,
                                   BodyComposition{ 0.0f, 0.0f, 0.0f, 1.0f }, gas);
                c->mass += gas;
                c->giant = TRUE;
                evo_log(out, (u8)epoch, EVO_EV_GIANT_FORMED, (i8)i, -1);
            }

            // Type-II migration: giants surf the gas disk inward while it lasts.
            if (c->giant && epoch < dispersal_epoch) {
                f32 a_new = c->a * galaxy_rng_range(&r, 0.80f, 0.90f);
                c->a = a_new < au_min * 0.55f ? au_min * 0.55f : a_new;
                if (!c->migrated) {
                    c->migrated = TRUE;
                    evo_log(out, (u8)epoch, EVO_EV_MIGRATED, (i8)i, -1);
                }
            }
            // Type-I migration: heavy cores drift slowly inward through the gas.
            else if (!c->giant && c->mass > 1.5f && epoch < dispersal_epoch) {
                f32 a_new = c->a * galaxy_rng_range(&r, 0.88f, 0.98f);
                c->a = a_new < au_min * 0.55f ? au_min * 0.55f : a_new;
            }
        }

        // --- giants stir nearby annuli, starving neighbours of feedstock ---
        for (i32 i = 0; i < n; ++i) {
            ProtoCore* c = &cores[i];
            if (!c->alive || c->is_belt || c->giant) continue;
            for (i32 j = 0; j < n; ++j) {
                ProtoCore* g = &cores[j];
                if (!g->alive || !g->giant) continue;
                f32 rh = hill_radius_au(g->a, g->mass, star.mass_solar);
                if (fabsf(g->a - c->a) < 12.0f * rh) { c->feed *= 0.25f; break; }
            }
        }

        // --- Hill-stability pass over adjacent alive pairs (sorted by a) ---
        i32 alive_n = 0;
        for (i32 i = 0; i < n; ++i)
            if (cores[i].alive && !cores[i].is_belt) order[alive_n++] = i;
        for (i32 k = 1; k < alive_n; ++k) { // insertion sort by a (n <= 10)
            i32 v = order[k]; i32 j = k - 1;
            while (j >= 0 && cores[order[j]].a > cores[v].a) { order[j + 1] = order[j]; --j; }
            order[j + 1] = v;
        }

        for (i32 k = 0; k + 1 < alive_n; ++k) {
            ProtoCore* lo = &cores[order[k]];
            ProtoCore* hi = &cores[order[k + 1]];
            if (!lo->alive || !hi->alive) continue;
            f32 a_mid   = 0.5f * (lo->a + hi->a);
            f32 mutual  = hill_radius_au(a_mid, lo->mass + hi->mass, star.mass_solar);
            if (hi->a - lo->a >= 6.0f * mutual) continue;

            // Unstable pair. Lighter body's stream decides its fate.
            ProtoCore* big   = lo->mass >= hi->mass ? lo : hi;
            ProtoCore* small = lo->mass >= hi->mass ? hi : lo;
            i32 small_idx = (i32)(small - cores);
            i32 big_idx   = (i32)(big - cores);
            GalaxyRng r = evo_rng(seed, small_idx, epoch + 100); // fate stream, distinct from growth
            b8 eject = big->mass > 3.0f * small->mass && big->giant
                    && galaxy_rng_f32(&r) < 0.45f && alive_n > 3;
            if (eject) {
                small->alive = FALSE;
                evo_log(out, (u8)epoch, EVO_EV_EJECTED, (i8)small_idx, (i8)big_idx);
            } else {
                big->comp = comp_mix(big->comp, big->mass, small->comp, small->mass);
                big->a    = (big->a * big->mass + small->a * small->mass) / (big->mass + small->mass);
                big->mass += small->mass;
                big->feed += small->feed;
                small->alive = FALSE;
                evo_log(out, (u8)epoch, EVO_EV_MERGER, (i8)big_idx, (i8)small_idx);
                // A late giant impact on a rocky survivor can spin off a moon.
                if (!big->giant && big->mass > 0.3f && big->mass < 8.0f
                    && epoch >= dispersal_epoch && galaxy_rng_f32(&r) < 0.65f) {
                    big->impact_moon = TRUE;
                    evo_log(out, (u8)epoch, EVO_EV_MOON_IMPACT, (i8)big_idx, -1);
                }
            }
            alive_n = 0; // membership changed; recount next epoch (one resolution per epoch pass is fine)
            break;
        }

        // --- belt formation: runts starved next to a giant grind down instead of growing ---
        i32 belts = 0;
        for (i32 i = 0; i < n; ++i) if (cores[i].is_belt) ++belts;
        if (belts < MAX_SYSTEM_BELTS && epoch >= dispersal_epoch) {
            for (i32 i = 0; i < n && belts < MAX_SYSTEM_BELTS; ++i) {
                ProtoCore* c = &cores[i];
                if (!c->alive || c->is_belt || c->giant || c->mass >= 3.0f) continue;
                // Frustrated only if a nearby giant keeps stirring it.
                for (i32 j = 0; j < n; ++j) {
                    ProtoCore* g = &cores[j];
                    if (!g->alive || !g->giant || c->mass >= 0.05f * g->mass) continue;
                    f32 rh = hill_radius_au(g->a, g->mass, star.mass_solar);
                    if (fabsf(g->a - c->a) < 14.0f * rh) {
                        c->is_belt = TRUE;
                        c->belt_width = clampf(4.0f * hill_radius_au(c->a, c->mass + c->feed,
                                                                     star.mass_solar),
                                               0.04f * c->a, 0.22f * c->a);
                        evo_log(out, (u8)epoch, EVO_EV_BELT_FORMED, (i8)i, (i8)j);
                        ++belts;
                        break;
                    }
                }
            }
        }
    }
}

// =====================================================================================
// Phase 2.5 — build the final body array: [0]=star, planets sorted by a, moons, belts.
// Enforces 2..MAX_SYSTEM_PLANETS planets and stable index order for all consumers.
// =====================================================================================
static void build_body_array(ProtoCore* cores, i32 n, const StarProperties& star,
                             u64 seed, EvolvedSystem* out) {
    // Star at slot 0.
    EvolvedBody* sb = &out->bodies[0];
    *sb = EvolvedBody{};
    sb->kind = BODY_STAR;
    sb->parent = -1;
    sb->mass_earth = star.mass_solar * EARTHS_PER_SOLAR;
    sb->radius_earth = star.radius_solar * 109.2f;
    sb->temperature_k = star.temperature_k;
    sb->comp = BodyComposition{ 0.0f, 0.0f, 0.0f, 1.0f };

    // Collect surviving planets sorted by a.
    i32 order[EVO_MAX_CORES]; i32 pc = 0;
    for (i32 i = 0; i < n; ++i)
        if (cores[i].alive && !cores[i].is_belt) order[pc++] = i;
    for (i32 k = 1; k < pc; ++k) {
        i32 v = order[k]; i32 j = k - 1;
        while (j >= 0 && cores[order[j]].a > cores[v].a) { order[j + 1] = order[j]; --j; }
        order[j + 1] = v;
    }
    // Too many survivors: merge the lightest into its nearest neighbour until it fits.
    while (pc > MAX_SYSTEM_PLANETS) {
        i32 lightest = 0;
        for (i32 k = 1; k < pc; ++k)
            if (cores[order[k]].mass < cores[order[lightest]].mass) lightest = k;
        i32 nb = lightest > 0 ? lightest - 1 : 1;
        ProtoCore* dst = &cores[order[nb]];
        ProtoCore* src = &cores[order[lightest]];
        dst->comp = comp_mix(dst->comp, dst->mass, src->comp, src->mass);
        dst->mass += src->mass;
        src->alive = FALSE;
        for (i32 k = lightest; k + 1 < pc; ++k) order[k] = order[k + 1];
        --pc;
    }
    // Too few (heavy ejection runs): backfill deterministic rocky worlds near the HZ.
    GalaxyRng fill = evo_rng(seed, -1, 2);
    while (pc < 2) {
        i32 slot = -1;
        for (i32 i = 0; i < n; ++i)
            if (!cores[i].alive && !cores[i].is_belt) { slot = i; break; }
        if (slot < 0) break;
        ProtoCore* c = &cores[slot];
        c->alive = TRUE;
        c->a = star.hz_inner_au * galaxy_rng_range(&fill, 0.8f, 1.6f) * (pc == 0 ? 1.0f : 1.9f);
        c->e = galaxy_rng_range(&fill, 0.0f, 0.08f);
        c->mass = galaxy_rng_range(&fill, 0.2f, 1.5f);
        c->comp = BodyComposition{ 0.3f, 0.7f, 0.0f, 0.0f };
        c->giant = FALSE;
        order[pc++] = slot;
    }

    // Planets into slots [1..pc].
    out->planet_count = pc;
    for (i32 k = 0; k < pc; ++k) {
        ProtoCore* c = &cores[order[k]];
        EvolvedBody* b = &out->bodies[1 + k];
        *b = EvolvedBody{};
        b->kind = BODY_PLANET;
        b->parent = 0;
        b->orbit_au = c->a;
        b->eccentricity = clampf(c->e, 0.0f, 0.25f);
        b->mass_earth = c->mass;
        b->comp = comp_normalize(c->comp);
        // Patch fate-relevant flags through for the moon pass below.
        b->volcanism = c->impact_moon ? 1.0f : 0.0f; // temp scratch, overwritten in phase 3
    }

    // Moons: impact moons on flagged rockies, captured moons on giants.
    i32 mc = 0;
    i32 moon_base = 1 + pc;
    for (i32 k = 0; k < pc && mc < MAX_SYSTEM_MOONS; ++k) {
        ProtoCore* c = &cores[order[k]];
        EvolvedBody* p = &out->bodies[1 + k];
        GalaxyRng r = evo_rng(seed, order[k], 200); // moon stream
        i32 want = 0;
        b8 captured = FALSE;
        if (c->giant) {
            want = galaxy_rng_int(&r, 1, 2);
            captured = TRUE;
        } else if (c->impact_moon) {
            want = 1;
        }
        f32 rh = hill_radius_au(p->orbit_au, p->mass_earth, star.mass_solar);
        for (i32 m = 0; m < want && mc < MAX_SYSTEM_MOONS; ++m) {
            EvolvedBody* mb = &out->bodies[moon_base + mc];
            *mb = EvolvedBody{};
            mb->kind = BODY_MOON;
            mb->parent = (i8)(1 + k);
            mb->orbit_au = rh * galaxy_rng_range(&r, 0.08f, 0.35f); // safely inside the Hill sphere
            mb->eccentricity = galaxy_rng_range(&r, 0.0f, 0.05f);
            mb->mass_earth = captured
                ? p->mass_earth * galaxy_rng_range(&r, 0.00002f, 0.0003f)
                : p->mass_earth * galaxy_rng_range(&r, 0.008f, 0.02f);
            mb->comp = captured
                ? BodyComposition{ 0.08f, 0.35f, 0.57f, 0.0f }  // captured: icy outer material
                : comp_normalize(BodyComposition{ p->comp.metal * 0.3f, p->comp.silicate + p->comp.metal * 0.7f, p->comp.ice, 0.0f });
            if (captured)
                evo_log(out, EVO_ACCRETION_EPOCHS, EVO_EV_MOON_CAPTURED, (i8)(moon_base + mc), mb->parent);
            ++mc;
        }
    }
    out->moon_count = mc;

    // Belts after moons.
    i32 bc = 0;
    i32 belt_base = moon_base + mc;
    for (i32 i = 0; i < n && bc < MAX_SYSTEM_BELTS; ++i) {
        ProtoCore* c = &cores[i];
        if (!c->is_belt) continue;
        EvolvedBody* b = &out->bodies[belt_base + bc];
        *b = EvolvedBody{};
        b->kind = BODY_BELT;
        b->parent = 0;
        b->orbit_au = c->a;
        b->width_au = c->belt_width;
        b->mass_earth = c->mass + c->feed * 0.3f; // ground-down remnant mass
        b->comp = comp_normalize(c->comp);
        ++bc;
    }
    out->belt_count = bc;
    out->body_count = belt_base + bc;

    // Remap accretion-phase chronicle entries from ProtoCore slot indices to final bodies[]
    // indices so the per-planet chronicle (UI) can attribute them. Cores that did not survive
    // as a planet or belt map to -2 ("a lost protoplanet"). MOON_CAPTURED already logs final
    // indices, and phase-3/4 events are logged after this point, so only the listed kinds remap.
    {
        i8 core_to_body[EVO_MAX_CORES];
        for (i32 i = 0; i < n; ++i) core_to_body[i] = -2;
        for (i32 k = 0; k < pc; ++k) core_to_body[order[k]] = (i8)(1 + k);
        i32 bi = 0;
        for (i32 i = 0; i < n && bi < bc; ++i)
            if (cores[i].is_belt) core_to_body[i] = (i8)(belt_base + bi++);
        for (i32 e = 0; e < out->event_count; ++e) {
            EvolutionEvent* ev = &out->events[e];
            if (ev->kind != EVO_EV_GIANT_FORMED && ev->kind != EVO_EV_MIGRATED
                && ev->kind != EVO_EV_MERGER && ev->kind != EVO_EV_EJECTED
                && ev->kind != EVO_EV_BELT_FORMED && ev->kind != EVO_EV_MOON_IMPACT) continue;
            if (ev->body  >= 0 && ev->body  < (i8)n) ev->body  = core_to_body[ev->body];
            if (ev->other >= 0 && ev->other < (i8)n) ev->other = core_to_body[ev->other];
        }
    }
}

// =====================================================================================
// Phase 3 — geophysical & atmospheric evolution. Epochs march through the star's age:
// differentiation, decaying radiogenic tectonics, outgassing vs escape, bombardment
// water delivery, greenhouse temperature, slow biosphere accumulation.
// =====================================================================================
static void phase3_geophysics(EvolvedSystem* out, const StarProperties& star, u64 seed) {
    f32 age_step = star.age_gyr / (f32)EVO_GEO_EPOCHS;
    // XUV/flare harshness: cool M/K dwarfs stay violently active far longer.
    f32 star_xuv = star.temperature_k < 3900.0f ? 0.55f
                 : star.temperature_k < 5300.0f ? 0.22f : 0.10f;

    // Outer ice reservoir feeds the epoch-1 bombardment (icy planets + belts beyond frost).
    f32 ice_reservoir = 0.0f;
    for (i32 i = 1; i < out->body_count; ++i) {
        EvolvedBody* b = &out->bodies[i];
        if (b->kind == BODY_MOON) continue;
        if (b->orbit_au > star.frost_line_au) ice_reservoir += b->mass_earth * b->comp.ice;
    }
    ice_reservoir = clampf(ice_reservoir, 0.0f, 60.0f);

    for (i32 i = 1; i < out->body_count; ++i) {
        EvolvedBody* b = &out->bodies[i];
        if (b->kind == BODY_BELT) continue;
        EvolvedBody* parent = &out->bodies[b->parent];
        b8  is_moon  = b->kind == BODY_MOON;
        b8  is_giant = b->comp.gas > 0.35f;
        f32 a_star   = is_moon ? parent->orbit_au : b->orbit_au; // distance from the star
        f32 t_eq     = 255.0f * powf(star.luminosity_solar, 0.25f) / sqrtf(a_star);
        f32 m        = b->mass_earth;

        b->water_frac = 0.0f; b->atmo_pressure = 0.0f; b->magnetic_field = 0.0f;
        b->tectonics = 0.0f;  b->volcanism = 0.0f;     b->life = 0.0f;
        b->temperature_k = t_eq;

        if (is_giant) {
            // Giants: massive envelopes, strong dynamos; skip the rocky loop.
            b->atmo_pressure = 1000.0f;
            b->magnetic_field = clamp01(0.5f + m / 400.0f);
            b->temperature_k = t_eq;
            continue;
        }

        // Tidal heating for close-in moons of massive parents.
        f32 rh = hill_radius_au(parent->orbit_au, parent->mass_earth, star.mass_solar);
        f32 tidal = 0.0f;
        if (is_moon && rh > 1e-6f)
            tidal = clamp01(0.35f * (parent->mass_earth / 100.0f) * (1.0f - b->orbit_au / rh))
                  * (parent->comp.gas > 0.35f ? 1.0f : 0.25f);

        b8 was_thick = FALSE, stripped_logged = FALSE, life_logged = FALSE, water_logged = FALSE;

        // Cooling timescale grows with mass: small bodies (Mars) freeze out in a couple of
        // Gyr while super-Earths stay geologically active for the age of the galaxy.
        f32 cool_tau = 2.0f + 2.5f * sqrtf(clampf(m, 0.05f, 12.0f));

        for (i32 e = 0; e < EVO_GEO_EPOCHS; ++e) {
            i32 epoch_abs = 1 + EVO_ACCRETION_EPOCHS + e;
            GalaxyRng r = evo_rng(seed, i, epoch_abs + 300);
            f32 age = ((f32)e + 1.0f) * age_step;

            if (e == 0) {
                // Differentiation: metals sink -> core -> dynamo (needs mass to stay molten).
                b->magnetic_field = clamp01(2.0f * b->comp.metal * sqrtf(clampf(m, 0.0f, 4.0f))
                                            * galaxy_rng_range(&r, 0.7f, 1.3f));
            }

            // Radiogenic + primordial heat decays with age; small bodies cool fastest.
            f32 heat = expf(-age / cool_tau) * clamp01(sqrtf(m) * 1.1f);
            b->tectonics = clamp01(heat * galaxy_rng_range(&r, 0.8f, 1.2f));
            b->volcanism = clamp01(heat * galaxy_rng_range(&r, 0.6f, 1.3f) + tidal);
            if (m < 0.5f) b->magnetic_field *= 0.75f; // small cores freeze out

            // Atmosphere: volcanic outgassing vs thermal Jeans escape + XUV stripping.
            f32 outgas = b->volcanism * (b->comp.silicate * 0.8f + b->comp.ice * 1.9f)
                       * clampf(m, 0.0f, 2.0f);
            f32 jeans  = clamp01((t_eq / 350.0f) / (m + 0.15f)) * 0.30f;
            f32 xuv    = star_xuv / (a_star * a_star + 0.05f);
            if (e >= 2) xuv *= 0.35f; // young stars flare hardest
            f32 escape = clamp01(jeans + clamp01(xuv) * (1.0f - b->magnetic_field * 0.7f));
            b->atmo_pressure = b->atmo_pressure * (1.0f - escape * 0.65f) + outgas;
            if (b->atmo_pressure > 0.3f) was_thick = TRUE;
            if (was_thick && b->atmo_pressure < 0.05f && !stripped_logged) {
                stripped_logged = TRUE;
                evo_log(out, (u8)epoch_abs, EVO_EV_ATMO_STRIPPED, (i8)i, -1);
            }

            // Epoch 1: late heavy bombardment delivers outer-system ice to inner worlds.
            if (e == 1 && !is_moon) {
                f32 delivered = clamp01(0.08f * sqrtf(ice_reservoir) / (a_star + 0.2f))
                              * galaxy_rng_range(&r, 0.5f, 1.5f);
                b->water_frac = clamp01(b->comp.ice * 1.8f + delivered);
                if (delivered > 0.06f && !water_logged && t_eq < 330.0f) {
                    water_logged = TRUE;
                    evo_log(out, (u8)epoch_abs, EVO_EV_WATER_DELIVERED, (i8)i, -1);
                }
            } else if (e == 1) {
                b->water_frac = clamp01(b->comp.ice * 1.8f);
            }
            // Hot worlds boil their water off unless a thick atmosphere holds steam.
            if (b->temperature_k > 380.0f && b->atmo_pressure < 2.0f)
                b->water_frac *= 0.25f;

            // Greenhouse-adjusted temperature.
            b->temperature_k = t_eq * (1.0f + 0.14f * logf(1.0f + b->atmo_pressure));

            // Life: liquid-water worlds with air accumulate a biosphere over the ages.
            b8 suitable = b->water_frac > 0.15f
                       && b->temperature_k > 240.0f && b->temperature_k < 330.0f
                       && b->atmo_pressure > 0.2f && b->atmo_pressure < 8.0f;
            if (suitable) {
                f32 shelter = 0.5f + 0.5f * b->magnetic_field;
                b->life = clamp01(b->life + 0.22f * age_step * shelter);
                if (b->life > 0.3f && !life_logged) {
                    life_logged = TRUE;
                    evo_log(out, (u8)epoch_abs, EVO_EV_LIFE_EMERGED, (i8)i, -1);
                }
            }
        }

        // Dynamo needs a molten, convecting core: as the interior heat dies the magnetic
        // field decays with it (a cooled-out world can't keep a Strong field).
        f32 final_heat = expf(-star.age_gyr / cool_tau) * clamp01(sqrtf(m) * 1.1f);
        b->magnetic_field *= clamp01(0.25f + 1.4f * final_heat);
    }
}

// =====================================================================================
// Phase 4 — present-day synthesis. Classify PlanetType from the evolved state (the render
// contract), derive radius, habitability, env_hazard, and resource richness.
// =====================================================================================
static PlanetType classify_body(const EvolvedBody* b) {
    if (b->comp.gas > 0.35f)
        return (b->mass_earth > 40.0f && b->comp.ice < 0.25f) ? PLANET_GAS_GIANT : PLANET_ICE_GIANT;
    f32 t = b->temperature_k;
    if (t > 700.0f || (b->volcanism > 0.8f && t > 450.0f)) return PLANET_LAVA;
    if (t < 200.0f) return PLANET_FROZEN;
    if (b->water_frac > 0.55f && b->atmo_pressure > 0.3f)  return PLANET_OCEAN;
    if (b->water_frac > 0.15f && b->atmo_pressure > 0.25f
        && t > 240.0f && t < 330.0f)                       return PLANET_TERRAN;
    if (t > 250.0f && b->atmo_pressure > 0.05f && b->water_frac < 0.15f) return PLANET_DESERT;
    return PLANET_ROCKY;
}

static void phase4_synthesis(EvolvedSystem* out, const StarProperties& star, u64 seed) {
    u8 final_epoch = 1 + EVO_ACCRETION_EPOCHS + EVO_GEO_EPOCHS; // "today"
    (void)final_epoch;

    for (i32 i = 1; i < out->body_count; ++i) {
        EvolvedBody* b = &out->bodies[i];
        GalaxyRng r = evo_rng(seed, i, 500); // synthesis stream

        if (b->kind == BODY_BELT) {
            b->res_metal     = clamp01(b->comp.metal * 2.2f * galaxy_rng_range(&r, 0.7f, 1.3f));
            b->res_volatiles = clamp01(b->comp.ice * 1.6f * galaxy_rng_range(&r, 0.7f, 1.3f));
            b->env_hazard    = clamp01(0.25f + b->mass_earth * 0.15f); // collision flux
            continue;
        }

        b->type = classify_body(b);

        // Radius from mass + composition.
        f32 m = b->mass_earth;
        if (b->comp.gas > 0.35f) {
            b->radius_earth = m > 40.0f ? clampf(8.0f + m * 0.008f, 8.0f, 13.0f)
                                        : clampf(3.0f + m * 0.09f, 3.0f, 6.5f);
        } else {
            f32 ice_puff = 1.0f + 0.25f * b->comp.ice;
            b->radius_earth = powf(clampf(m, 0.005f, 20.0f), 0.27f) * ice_puff;
        }

        // Habitability: multiplicative gates on the evolved state.
        f32 hab = 0.0f;
        if (b->type == PLANET_TERRAN || b->type == PLANET_OCEAN) {
            f32 temp_fit  = 1.0f - clamp01(fabsf(b->temperature_k - 288.0f) / 55.0f);
            f32 atmo_fit  = 1.0f - clamp01(fabsf(logf(b->atmo_pressure + 0.01f)) / 2.5f);
            f32 water_fit = clamp01(b->water_frac / 0.3f) * (b->type == PLANET_OCEAN ? 0.85f : 1.0f);
            f32 shelter   = 0.55f + 0.45f * b->magnetic_field;
            hab = clamp01(temp_fit * atmo_fit * water_fit * shelter * (0.6f + 0.4f * b->life));
        }
        b->habitability = hab;

        // Environmental hazard: radiation (bare + close to an active star), impact flux
        // near a belt, extreme volcanism, crushing/corrosive atmospheres.
        f32 a_star = b->kind == BODY_MOON ? out->bodies[b->parent].orbit_au : b->orbit_au;
        f32 star_xuv = star.temperature_k < 3900.0f ? 0.55f
                     : star.temperature_k < 5300.0f ? 0.22f : 0.10f;
        f32 radiation = clamp01(star_xuv / (a_star * a_star + 0.05f))
                      * (1.0f - b->magnetic_field * 0.8f);
        f32 impacts = 0.0f;
        for (i32 j = 0; j < out->body_count; ++j) {
            const EvolvedBody* belt = &out->bodies[j];
            if (belt->kind != BODY_BELT) continue;
            f32 d = fabsf(belt->orbit_au - a_star);
            impacts = clamp01(impacts + clamp01(1.0f - d / (belt->width_au * 6.0f + 0.01f)) * 0.5f);
        }
        f32 volc_hz = clamp01(b->volcanism - 0.5f) * 1.4f;
        f32 atmo_hz = b->comp.gas > 0.35f ? 1.0f
                    : clamp01((b->atmo_pressure - 5.0f) / 40.0f);
        f32 hz = radiation;
        if (impacts > hz) hz = impacts;
        if (volc_hz > hz) hz = volc_hz;
        if (atmo_hz > hz) hz = atmo_hz;
        b->env_hazard = clamp01(hz + 0.15f * (radiation + impacts + volc_hz + atmo_hz - hz));

        // Resource richness (data only; market untouched). Volcanism concentrates ores.
        b->res_metal     = clamp01(b->comp.metal * (1.2f + b->volcanism)
                                   * galaxy_rng_range(&r, 0.7f, 1.3f));
        b->res_volatiles = clamp01((b->comp.ice + b->water_frac * 0.5f)
                                   * galaxy_rng_range(&r, 0.7f, 1.3f));
    }
}

// =====================================================================================
// Entry point
// =====================================================================================
void evolve_star_system(EvolvedSystem* out, const StarProperties& star, u64 seed) {
    *out = EvolvedSystem{};
    ProtoCore cores[EVO_MAX_CORES];
    i32 n = phase1_disk(cores, star, seed);
    phase2_accretion(cores, n, star, seed, out);
    build_body_array(cores, n, star, seed, out);
    phase3_geophysics(out, star, seed);
    phase4_synthesis(out, star, seed);
}

const char* evo_event_name(u8 kind) {
    switch (kind) {
        case EVO_EV_DISK_DISPERSED:  return "gas disk dispersed";
        case EVO_EV_GIANT_FORMED:    return "runaway gas accretion";
        case EVO_EV_MIGRATED:        return "giant migrated inward";
        case EVO_EV_MERGER:          return "protoplanets merged";
        case EVO_EV_EJECTED:         return "body ejected";
        case EVO_EV_BELT_FORMED:     return "asteroid belt formed";
        case EVO_EV_MOON_IMPACT:     return "impact-formed moon";
        case EVO_EV_MOON_CAPTURED:   return "moon captured";
        case EVO_EV_ATMO_STRIPPED:   return "atmosphere stripped";
        case EVO_EV_WATER_DELIVERED: return "water delivered";
        case EVO_EV_LIFE_EMERGED:    return "life emerged";
        default:                     return "unknown";
    }
}

// =====================================================================================
// Self-test: per-phase statistics + invariants over a seed batch, logged via BS_LOG_INFO.
// =====================================================================================
static b8 evo_check_finite(const EvolvedBody* b) {
    const f32 v[] = { b->orbit_au, b->eccentricity, b->mass_earth, b->radius_earth,
                      b->temperature_k, b->comp.metal, b->comp.silicate, b->comp.ice,
                      b->comp.gas, b->water_frac, b->atmo_pressure, b->magnetic_field,
                      b->tectonics, b->volcanism, b->life, b->habitability, b->env_hazard,
                      b->res_metal, b->res_volatiles };
    for (u32 k = 0; k < sizeof(v) / sizeof(v[0]); ++k)
        if (!isfinite(v[k])) return FALSE;
    return TRUE;
}

void system_evolution_selftest() {
    const u64 master = 0xB1AC5741DEull;
    const i32 N = 64;

    i32 total_planets = 0, total_moons = 0, total_belts = 0, total_giants = 0;
    i32 habitable = 0, with_life = 0, with_water = 0;
    i32 fail_comp = 0, fail_hill = 0, fail_moon = 0, fail_finite = 0, fail_count = 0, fail_det = 0;
    i32 massive_dead = 0, dead_strong_dynamo = 0;
    i32 type_hist[PLANET_TYPE_COUNT] = {};

    EvolvedSystem sys, sys2;
    for (i32 s = 0; s < N; ++s) {
        u64 seed = galaxy_seed_for(master, s);
        StarProperties star = worldgen_star(galaxy_splitmix64(seed ^ 0xA24BAED4963EE407ull));
        evolve_star_system(&sys, star, seed);

        // Determinism: same inputs twice -> bit-identical output.
        evolve_star_system(&sys2, star, seed);
        if (memcmp(&sys, &sys2, sizeof(EvolvedSystem)) != 0) ++fail_det;

        if (sys.planet_count < 2 || sys.planet_count > MAX_SYSTEM_PLANETS
            || sys.moon_count > MAX_SYSTEM_MOONS || sys.belt_count > MAX_SYSTEM_BELTS
            || sys.body_count != 1 + sys.planet_count + sys.moon_count + sys.belt_count)
            ++fail_count;

        total_planets += sys.planet_count;
        total_moons   += sys.moon_count;
        total_belts   += sys.belt_count;

        for (i32 i = 1; i < sys.body_count; ++i) {
            const EvolvedBody* b = &sys.bodies[i];
            if (!evo_check_finite(b)) ++fail_finite;
            f32 cs = b->comp.metal + b->comp.silicate + b->comp.ice + b->comp.gas;
            if (cs < 0.98f || cs > 1.02f) ++fail_comp;
            if (b->kind == BODY_PLANET) {
                ++type_hist[(i32)b->type];
                if (b->comp.gas > 0.35f) ++total_giants;
                if (b->habitability > 0.4f) ++habitable;
                if (b->life > 0.3f) ++with_life;
                if (b->water_frac > 0.15f) ++with_water;
                // Geophysics consistency: massive solid worlds shouldn't be geologically
                // dead, and dead worlds shouldn't keep a strong core dynamo.
                if (b->comp.gas <= 0.35f) {
                    if (b->mass_earth > 2.0f && b->tectonics < 0.1f) ++massive_dead;
                    if (b->tectonics < 0.15f && b->magnetic_field > 0.6f) ++dead_strong_dynamo;
                }
            }
            if (b->kind == BODY_MOON) {
                const EvolvedBody* p = &sys.bodies[b->parent];
                f32 rh = hill_radius_au(p->orbit_au, p->mass_earth, star.mass_solar);
                if (b->orbit_au > rh) ++fail_moon;
            }
        }
        // Hill spacing between adjacent planets (relaxed bound; migration compresses).
        for (i32 i = 1; i + 1 <= sys.planet_count; ++i) {
            const EvolvedBody* lo = &sys.bodies[i];
            const EvolvedBody* hi = &sys.bodies[i + 1];
            if (hi->orbit_au <= lo->orbit_au) { ++fail_hill; continue; }
            f32 mutual = hill_radius_au(0.5f * (lo->orbit_au + hi->orbit_au),
                                        lo->mass_earth + hi->mass_earth, star.mass_solar);
            if (hi->orbit_au - lo->orbit_au < 3.0f * mutual) ++fail_hill;
        }
    }

    BS_LOG_INFO("[evo selftest] %d systems: planets avg %.2f, moons avg %.2f, belts avg %.2f, giants %d",
                N, (f32)total_planets / N, (f32)total_moons / N, (f32)total_belts / N, total_giants);
    BS_LOG_INFO("[evo selftest] habitable(>0.4) %d/%d planets (%.1f%%), life %d, watery %d",
                habitable, total_planets, 100.0f * habitable / (f32)total_planets, with_life, with_water);
    BS_LOG_INFO("[evo selftest] types: lava %d rocky %d desert %d ocean %d terran %d gasG %d iceG %d frozen %d",
                type_hist[PLANET_LAVA], type_hist[PLANET_ROCKY], type_hist[PLANET_DESERT],
                type_hist[PLANET_OCEAN], type_hist[PLANET_TERRAN], type_hist[PLANET_GAS_GIANT],
                type_hist[PLANET_ICE_GIANT], type_hist[PLANET_FROZEN]);
    BS_LOG_INFO("[evo selftest] invariants: determinism_fail=%d count_fail=%d comp_fail=%d hill_fail=%d moon_fail=%d finite_fail=%d",
                fail_det, fail_count, fail_comp, fail_hill, fail_moon, fail_finite);
    BS_LOG_INFO("[evo selftest] consistency: massive_dead=%d dead_strong_dynamo=%d",
                massive_dead, dead_strong_dynamo);

    // Detailed chronicle for one seed (phase-by-phase visibility).
    u64 seed = galaxy_seed_for(master, 0);
    StarProperties star = worldgen_star(galaxy_splitmix64(seed ^ 0xA24BAED4963EE407ull));
    evolve_star_system(&sys, star, seed);
    BS_LOG_INFO("[evo trace] seed=%llx star: class=%s M=%.2f L=%.2f age=%.1fGyr Z=%.2f frost=%.2fAU",
                (unsigned long long)seed, spectral_class_name(star.spectral_class),
                star.mass_solar, star.luminosity_solar, star.age_gyr, star.metallicity,
                star.frost_line_au);
    for (i32 i = 1; i < sys.body_count; ++i) {
        const EvolvedBody* b = &sys.bodies[i];
        if (b->kind == BODY_BELT) {
            BS_LOG_INFO("[evo trace]  body %d BELT   a=%.2fAU w=%.2f mass=%.2fMe ore=%.2f volat=%.2f hazard=%.2f",
                        i, b->orbit_au, b->width_au, b->mass_earth, b->res_metal,
                        b->res_volatiles, b->env_hazard);
        } else {
            BS_LOG_INFO("[evo trace]  body %d %-6s a=%.2fAU m=%.2fMe r=%.2fRe T=%.0fK %s | atm=%.2f wat=%.2f mag=%.2f volc=%.2f life=%.2f hab=%.2f hz=%.2f",
                        i, b->kind == BODY_MOON ? "MOON" : "PLANET",
                        b->orbit_au, b->mass_earth, b->radius_earth, b->temperature_k,
                        planet_type_name(b->type), b->atmo_pressure, b->water_frac,
                        b->magnetic_field, b->volcanism, b->life, b->habitability, b->env_hazard);
        }
    }
    for (i32 e = 0; e < sys.event_count; ++e) {
        const EvolutionEvent* ev = &sys.events[e];
        BS_LOG_INFO("[evo trace]  epoch %2d: %s (body %d, other %d)",
                    ev->epoch, evo_event_name(ev->kind), ev->body, ev->other);
    }
}
