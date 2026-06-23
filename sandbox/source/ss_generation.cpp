#include "ss_generation.h"
#include <math.h>
using namespace bs_math;
// =====================================================================================
// Internal RNG (splitmix64)
// =====================================================================================
static u64 s_rng_state = 0x123456789ABCDEF0ull;
static void rng_seed(u64 seed) { s_rng_state = seed; }
static u64 rng_next() {
    u64 z = (s_rng_state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
static f32 rng_f32()  { return (f32)(rng_next() & 0xFFFFFF) / (f32)0xFFFFFF; }
static f32 rng_range(f32 min, f32 max) { return min + rng_f32() * (max - min); }
static i32 rng_int(i32 min, i32 max)   { return min + (i32)(rng_next() % (u64)(max - min + 1)); }
// =====================================================================================
const SSGenConfig SSGEN_DEFAULT{};
// =====================================================================================
f32 solve_eccentric_anomaly(f32 M, f32 e)
{
    // Normalize M to [0, 2π)
    f32 two_pi = 2.0f * BS_PI;
    M = fmodf(M, two_pi);
    if (M < 0.0f) M += two_pi;
    // Initial guess: E ≈ M + e * sin(M)  (good for e < 0.3)
    f32 E = M + e * sinf(M);
    // Newton-Raphson: E_{n+1} = E_n - (E_n - e*sin(E_n) - M) / (1 - e*cos(E_n))
    for (i32 iter = 0; iter < 8; ++iter) {
        f32 sinE = sinf(E);
        f32 cosE = cosf(E);
        f32 f  = E - e * sinE - M;
        f32 fp = 1.0f - e * cosE;
        f32 dE = f / fp;
        E -= dE;
        if (fabsf(dE) < 1e-6f) break;
    }
    return E;
}
// =====================================================================================
void generate_star_system(StarSystem* sys, u64 seed, Vec2 galaxy_pos,
                            const SSGenConfig& cfg)
{
    rng_seed(seed);
    // ---- Galaxy position (already computed by caller, just store it) ----
    sys->galaxy_center = hierpos_from_vec2(galaxy_pos, BS_HIERPOS_CELL_SIZE);
    // ---- Star: 4 color palettes, radius 900–1400 ----
    static const bs_color STAR_COLS[4] = {
        {1.00f, 0.95f, 0.40f, 1.0f}, // yellow
        {1.00f, 0.70f, 0.30f, 1.0f}, // orange
        {1.00f, 0.50f, 0.35f, 1.0f}, // red
        {0.75f, 0.85f, 1.00f, 1.0f}, // blue-white
    };
    i32 star_type = rng_int(0, 3);
    f32 star_r    = rng_range(900.0f, 1400.0f);
    sys->star = CelestialBody{ Vec2{ 0.0f, 0.0f }, star_r, STAR_COLS[star_type],
                                0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    sys->star_pulse_phase   = rng_f32() * 2.0f * BS_PI;
    sys->corona_pulse_phase = rng_f32() * 2.0f * BS_PI;
    sys->halo_pulse_phase   = rng_f32() * 2.0f * BS_PI;
    // ---- System scale and planet count ----
    sys->system_scale = rng_range(cfg.scale_min, cfg.scale_max);
    sys->planet_count = rng_int(cfg.min_planets, cfg.max_planets);
    // ---- Planet colors ----
    static const bs_color PLANET_COLS[6] = {
        {0.40f, 0.70f, 0.90f, 1.0f}, {0.85f, 0.55f, 0.30f, 1.0f},
        {0.40f, 0.80f, 0.45f, 1.0f}, {0.70f, 0.40f, 0.90f, 1.0f},
        {0.90f, 0.45f, 0.25f, 1.0f}, {0.50f, 0.80f, 0.85f, 1.0f},
    };
    // ---- Generate first planet ----
    f32 prev_a  = rng_range(cfg.inner_a_min, cfg.inner_a_max) * sys->system_scale;
    f32 prev_e  = rng_f32() * cfg.e_inner_max;
    f32 prev_M0 = rng_f32() * 2.0f * BS_PI;
    f32 prev_w  = rng_f32() * 2.0f * BS_PI;
    f32 planet_r = rng_range(250.0f, 600.0f);
    bs_color pcol = PLANET_COLS[rng_int(0, 5)];
    // Compute initial position using eccentric anomaly
    f32 E0 = solve_eccentric_anomaly(prev_M0, prev_e);
    f32 pos_x = prev_a * (cosf(E0) - prev_e);
    f32 pos_y = prev_a * sqrtf(1.0f - prev_e * prev_e) * sinf(E0);
    // Rotate by arg_periapsis
    f32 cw = cosf(prev_w);
    f32 sw = sinf(prev_w);
    Vec2 ppos = Vec2{ cw * pos_x - sw * pos_y, sw * pos_x + cw * pos_y };
    // Gameplay-scaled orbital speed: inner planet orbits in ~30s, outer in ~90s.
    // speed = K / sqrt(a/1e6)  where K tuned for visible motion.
    f32 speed  = 0.02f / sqrtf(prev_a / 1000000.0f);
    f32 period = 2.0f * BS_PI / speed;
    sys->planets[0] = CelestialBody{
        ppos, planet_r, pcol,
        prev_a,          // orbit_radius (compat) = semi_major_axis
        speed, prev_M0,  // orbit_speed, orbit_angle
        prev_a, prev_e, prev_w, prev_M0, period
    };
    // ---- Generate remaining planets ----
    for (i32 i = 1; i < sys->planet_count; ++i) {
        b8 accepted = FALSE;
        f32 a = 0.0f, e = 0.0f;
        for (i32 attempt = 0; attempt < 20; ++attempt) {
            f32 k   = rng_range(cfg.spacing_min, cfg.spacing_max);
            a       = prev_a * k;
            e       = rng_f32() * ((i < sys->planet_count / 2) ? cfg.e_inner_max : cfg.e_outer_max);
            // Validate non-intersection: apoapsis(prev) + margin < periapsis(this)
            f32 apo_prev  = prev_a * (1.0f + prev_e);
            f32 peri_this = a * (1.0f - e);
            if (apo_prev + cfg.safety_margin < peri_this) {
                accepted = TRUE;
                break;
            }
        }
        if (!accepted) {
            // Fallback: force a safe spacing with zero eccentricity
            a = prev_a * 2.0f + cfg.safety_margin;
            e = 0.0f;
        }
        prev_a = a;
        prev_e = e;
        f32 M0 = rng_f32() * 2.0f * BS_PI;
        f32 w  = rng_f32() * 2.0f * BS_PI;
        planet_r = rng_range(250.0f, 600.0f);
        pcol = PLANET_COLS[rng_int(0, 5)];
        E0 = solve_eccentric_anomaly(M0, e);
        pos_x = a * (cosf(E0) - e);
        pos_y = a * sqrtf(1.0f - e * e) * sinf(E0);
        cw = cosf(w);
        sw = sinf(w);
        ppos = Vec2{ cw * pos_x - sw * pos_y, sw * pos_x + cw * pos_y };
        speed  = 0.02f / sqrtf(a / 1000000.0f);
        period = 2.0f * BS_PI / speed;
        sys->planets[i] = CelestialBody{
            ppos, planet_r, pcol,
            a, speed, M0,
            a, e, w, M0, period
        };
    }
    // Clear any unused planet slots (if count < 5)
    for (i32 i = sys->planet_count; i < 5; ++i) {
        sys->planets[i] = CelestialBody{};
    }
}
// =====================================================================================
void update_planet_positions(StarSystem* sys, f32 dt)
{
    for (i32 i = 0; i < sys->planet_count; ++i) {
        CelestialBody& p = sys->planets[i];
        // Mean anomaly: M = M0 + ω * t
        f32 M = p.mean_anomaly_0 + p.orbital_period * dt;
        // Wait, orbit_speed is 2π/period, so:
        M = p.mean_anomaly_0 + p.orbit_speed * dt; // ... but this grows unbounded
        // Actually, use elapsed time from the planet's own tracking:
        // But we don't have elapsed time stored. The simplest approach:
        // orbit_angle was being used as the accumulated angle in the old code.
        // Let's use orbit_angle as the accumulated mean anomaly.
        p.orbit_angle += p.orbit_speed * dt;
        f32 E = solve_eccentric_anomaly(p.orbit_angle, p.eccentricity);
        // Position in orbital plane (periapsis on +x axis)
        f32 x = p.semi_major_axis * (cosf(E) - p.eccentricity);
        f32 y = p.semi_major_axis * sqrtf(1.0f - p.eccentricity * p.eccentricity) * sinf(E);
        // Rotate by arg_periapsis
        f32 cw = cosf(p.arg_periapsis);
        f32 sw = sinf(p.arg_periapsis);
        p.position.x = sys->star.position.x + cw * x - sw * y;
        p.position.y = sys->star.position.y + sw * x + cw * y;
    }
}
