// planet_surface.frag.hlsl — real-time procedural planet as an impostor sphere.
//
// Renders a lit sphere onto a screen-space quad (same quad vertex shader as the star): the sphere
// normal is reconstructed from the quad coords, shaded by the star's direction (day/night
// terminator -> orbital phases), and textured per PLANET TYPE (lava/rocky/desert/ocean/terran/
// gas giant/ice giant/frozen). Optional cloud layer, atmosphere rim glow, and a ring band.
// Output is PREMULTIPLIED alpha for the premult-over pipeline (out = src + dst*(1-src.a)).

cbuffer PlanetSurfaceParams : register(b0, space3)
{
    float4 a; // xy = centre px, z = body(disc) radius px, w = outer quad radius px
    float4 b; // xyz = base surface colour (legacy tint), w = elapsed time
    float4 c; // xy = light dir (screen space, normalized), z = planet type (float), w = visibility
    float4 d; // xy = framebuffer size px, z = rotation phase, w = surface seed
    float4 e; // x = has_atmosphere, y = has_rings, z = cloud amount, w = star elevation (rings)
    float4 f; // xyz = star light colour, w = reserved
    // ---- Per-planet genome (data-driven surface) ----
    float4 g_deep;   // xyz = palette deep,   w = noise_freq
    float4 g_mid;    // xyz = palette mid,    w = warp_amount
    float4 g_light;  // xyz = palette light,  w = feature_density
    float4 g_accent; // xyz = palette accent, w = band_detail
    float4 g_cloud;  // xyz = cloud tint,     w = cap_extent
    float4 g_atmo;   // xyz = atmosphere tint,w = roughness
    float4 g_misc;   // x = anomaly id, yzw = reserved
};

// Bundle of per-planet genome parameters passed into planet_albedo so the surface look is fully
// data-driven (palette + feature genes) instead of hardcoded per type.
struct PlanetGenes
{
    float3 deep, mid, light, accent;
    float  noise_freq, warp_amount, feature_density, band_detail, cap_extent, roughness, anomaly;
};

struct PSInput
{
    float4 position  : SV_Position;
    float2 local_pos : TEXCOORD0;
};

// --- hash / value-noise / fbm (same family as star_surface.frag) ------------------------
float hash(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}
float vnoise(float2 p)
{
    float2 i = floor(p);
    float2 fp = frac(p);
    fp = fp * fp * (3.0 - 2.0 * fp);
    float aa = hash(i);
    float bb = hash(i + float2(1.0, 0.0));
    float cc = hash(i + float2(0.0, 1.0));
    float dd = hash(i + float2(1.0, 1.0));
    return lerp(lerp(aa, bb, fp.x), lerp(cc, dd, fp.x), fp.y);
}
float fbm(float2 p)
{
    float v = 0.0, amp = 0.5, freq = 1.0;
    for (int i = 0; i < 5; ++i) { v += amp * vnoise(p * freq); amp *= 0.5; freq *= 2.03; }
    return v; // ~0..1
}
// Two-channel fbm + domain warp: sampling noise at a position that is itself displaced by noise
// gives the swirling, marbled, fluid look of gas-giant bands and organic coastlines (instead of
// blobby smoothstep thresholds). `amt` is the warp strength in the same units as `p`.
float2 fbm2(float2 p) { return float2(fbm(p), fbm(p + float2(3.7, 1.9))); }
float2 domain_warp(float2 p, float amt) { return p + amt * (fbm2(p) * 2.0 - 1.0); }

// Fractal surface mask: a low-frequency base broken up by progressively finer octaves so that
// patch/continent EDGES are fractal (coastline-like) instead of smooth single-scale blobs. This
// is the key to killing the "cartoonish" thresholded-noise look on terrestrial worlds. ~0..1.
float surface_mask(float2 p)
{
    float m = fbm(p);
    m += 0.32 * (vnoise(p * 3.7) - 0.5);   // mid-scale edge break-up
    m += 0.15 * (vnoise(p * 9.1) - 0.5);   // fine-scale roughness
    return saturate(m);
}

// Anti-aliased dark ring division: a Gaussian gap centred at `c` (in 0..1 ring param) whose sigma
// is broadened by the on-screen pixel footprint `rwt`, so a Cassini-style division never aliases
// into a sparkling seam when the rings are small on screen. Returns a multiplier in (1-depth .. 1].
float ring_gap(float t, float c, float sigma, float depth, float rwt)
{
    float s = sqrt(sigma * sigma + rwt * rwt);
    float g = exp(-((t - c) * (t - c)) / (2.0 * s * s));
    return 1.0 - depth * g;
}

// Per-type surface albedo from spherical UV, driven by the per-planet GENOME (4-stop palette +
// feature genes) so no two planets of a type look alike. The per-type STRUCTURE is preserved
// (lava cracks / rocky craters / dunes / continents+caps / gas+ice bands / frozen), but every
// colour and tuning value comes from `gn`. `em` returns emissive (lava glow).
float3 planet_albedo(int ptype, float2 uv, float lat, PlanetGenes gn, float seed, out float em)
{
    em = 0.0;
    float2 s = uv + seed;
    float  nf = gn.noise_freq;
    float  wa = gn.warp_amount;
    if (ptype == 0) {                       // LAVA — fractal molten crust w/ glowing plate cracks
        float2 w   = domain_warp(s * (2.2 * nf), 0.90 * wa);
        float  m   = surface_mask(w * 1.5);
        float  thr = 0.60 - 0.12 * gn.feature_density;
        float  hot = smoothstep(thr - 0.14, thr + 0.10, m);
        float3 crust  = gn.deep * (0.60 + 0.70 * vnoise(w * 7.0));           // dark, tonally varied
        float3 molten = lerp(gn.accent, gn.light, smoothstep(0.45, 1.0, m)); // hotter cores brighter
        float  crack  = smoothstep(0.05, 0.0, abs(m - thr));                 // thin glowing plate edges
        float3 col = lerp(crust, molten, hot);
        col = lerp(col, gn.accent, crack * (1.0 - hot));
        em  = hot * (0.85 + 0.90 * gn.feature_density) + crack * (1.0 - hot) * 0.9;
        return col;
    } else if (ptype == 1) {                // ROCKY — fractal rock + fine craters/grain
        float2 w = domain_warp(s * (2.6 * nf), 0.60 * wa);
        float  n = surface_mask(w * 1.4);
        float3 col = lerp(gn.deep, gn.light, saturate(0.20 + n * (0.7 + 0.5 * gn.roughness)));
        col *= 0.88 + 0.24 * vnoise(w * 9.0);                                // fine albedo grain
        float  craters = smoothstep(0.66 - 0.06 * gn.feature_density, 0.71, vnoise(w * 5.0))
                         * (0.18 + 0.40 * gn.feature_density);
        return col - craters * gn.mid;
    } else if (ptype == 2) {                // DESERT — layered dunes + sand grain
        float2 w = domain_warp(s * (2.2 * nf) + float2(0.0, sin(uv.x * 5.0)) * 0.22, 0.55 * wa);
        float  dunes = surface_mask(w * 1.3);
        float3 col = lerp(gn.deep, gn.light, saturate(0.28 + dunes * (0.7 + 0.4 * gn.roughness)));
        col *= 0.93 + 0.14 * vnoise(w * 10.0);                               // fine sand grain
        return col;
    } else if (ptype == 3 || ptype == 4) {  // OCEAN / TERRAN — fractal continents + caps
        float2 w = domain_warp(s * (1.8 * nf), 0.90 * wa);
        float  cont = surface_mask(w * 1.3);
        float  land = smoothstep(0.50 - 0.05 * gn.feature_density, 0.55, cont);
        float3 landc = (ptype == 4) ? gn.mid : lerp(gn.mid, gn.light, 0.35);
        landc *= 0.82 + 0.34 * vnoise(w * 6.0);                              // biome/terrain variation
        float3 col = lerp(gn.deep, landc, land);
        float  shallow = smoothstep(0.47 - 0.05 * gn.feature_density, 0.50, cont) * (1.0 - land);
        col = lerp(col, gn.light, shallow * 0.5);
        float  capT = 1.34 - gn.cap_extent * 0.95;                           // bigger cap_extent -> lower band
        float  ice  = smoothstep(capT, capT + 0.30, abs(lat) + 0.06 * (surface_mask(w * 2.0) - 0.5));
        col = lerp(col, lerp(gn.light, float3(0.94, 0.96, 1.0), 0.7), saturate(ice));
        return col;
    } else if (ptype == 5) {                // GAS GIANT — fluid Jovian bands + storms
        float  bandf = 6.0 + 8.0 * gn.band_detail;
        float2 fl    = domain_warp(float2(uv.x * 0.35, lat * 2.2) + seed, 1.2 * wa);
        float  band  = 0.5 + 0.5 * sin(lat * bandf + (fl.y * 2.0 - 1.0) * 2.4);
        float  storm = fbm(domain_warp(float2(uv.x * 0.7, lat * 3.0) + seed * 1.7, 0.9 * wa));
        float3 col = lerp(gn.deep, gn.mid, smoothstep(0.0, 0.55, band));
        col = lerp(col, gn.light, smoothstep(0.55, 1.0, band));
        col = lerp(col, gn.accent, saturate(storm - (0.55 - 0.12 * gn.feature_density)) * (0.45 + 0.45 * gn.feature_density));
        return col;
    } else if (ptype == 6) {                // ICE GIANT — fluid blue bands (Neptune-like)
        float  bandf  = 5.0 + 7.0 * gn.band_detail;
        float2 fl     = domain_warp(float2(uv.x * 0.30, lat * 2.0) + seed, 1.1 * wa);
        float  band   = 0.5 + 0.5 * sin(lat * bandf + (fl.y * 2.0 - 1.0) * 2.0);
        float  streak = fbm(domain_warp(float2(uv.x * 0.6, lat * 2.6) + seed * 1.3, 0.85 * wa));
        float3 col = lerp(gn.deep, gn.mid, smoothstep(0.0, 0.55, band));
        col = lerp(col, gn.light, smoothstep(0.55, 1.0, band));
        col = lerp(col, gn.accent, saturate(streak - 0.55) * (0.4 + 0.4 * gn.feature_density));
        return col;
    }
    // FROZEN (7) — fractal icy palette with cracks + grain
    float2 w = domain_warp(s * (3.0 * nf), 0.55 * wa);
    float  icem = surface_mask(w * 1.3);
    float  cr   = smoothstep(0.52, 0.60, icem);
    float3 col  = lerp(gn.light, gn.deep, cr * (0.5 + 0.5 * gn.roughness));
    col *= 0.94 + 0.12 * vnoise(w * 8.0);
    return col;
}

float4 main(PSInput input) : SV_Target0
{
    float2 lp     = input.local_pos;    // -1..1 across the outer quad radius
    float  body_r = a.z;
    float  quad_r = a.w;
    float  time   = b.w;
    float2 Ldir   = c.xy;
    int    ptype  = (int)(c.z + 0.5);
    float  vis    = c.w;
    float  rot    = d.z;
    float  seed   = d.w;
    float  has_atmo = e.x;
    float  has_rings= e.y;
    float  cloud_amt= e.z;
    float  star_elev= e.w;   // sin(star elevation above the ring plane); orbit-driven (seasons)
    float3 starcol  = f.xyz;

    // Per-planet genome: 4-stop palette + feature genes (unpacked from the cbuffer). The
    // atmosphere / limb-haze tint now comes straight from the genome instead of a per-type const.
    PlanetGenes gn;
    gn.deep = g_deep.xyz; gn.mid = g_mid.xyz; gn.light = g_light.xyz; gn.accent = g_accent.xyz;
    gn.noise_freq = g_deep.w; gn.warp_amount = g_mid.w; gn.feature_density = g_light.w;
    gn.band_detail = g_accent.w; gn.cap_extent = g_cloud.w; gn.roughness = g_atmo.w;
    gn.anomaly = g_misc.x;
    float3 atmo_tint = g_atmo.xyz;

    // Sphere-space coords: -1..1 across the disc (body_r / quad_r fraction of the quad).
    float body_norm = max(body_r / max(quad_r, 1.0), 0.02);
    float2 sp = lp / body_norm;
    float  r2 = dot(sp, sp);
    float  r  = sqrt(r2);
    float  disc_aa = fwidth(r);         // screen-space pixel footprint of the disc edge (for AA)

    float3 out_rgb = float3(0.0, 0.0, 0.0);
    float  out_a   = 0.0;

    // ---- Rings ----
    // A ring system in the planet's equatorial plane, tilted toward the camera (Y foreshortened).
    // The NEAR arc (bottom of the disc) is composited IN FRONT of the sphere so it crosses the
    // planet's lower face; the FAR arc (top) is drawn first and the opaque sphere paints over it,
    // so the ring truly wraps the planet instead of floating as a detached halo around it.
    float  ring_inner = 1.28, ring_outer = 2.15;
    float  ring_tilt  = 0.42;
    float  ring_span  = ring_outer - ring_inner;
    float  ring_cov   = 0.0;          // ring coverage / alpha at this pixel
    float3 ring_rgb   = float3(0.0, 0.0, 0.0);
    bool   ring_near  = (sp.y < 0.0); // near arc crosses the lower disc; far arc hides behind the top
    // ring_r and its screen-space pixel footprint are computed unconditionally so the derivative
    // (fwidth) stays well-defined and the analytic anti-aliasing below is stable at every zoom.
    float  ring_r  = sqrt(sp.x * sp.x + (sp.y / ring_tilt) * (sp.y / ring_tilt));
    float  ring_rw = max(fwidth(ring_r), 1e-5);
    if (has_rings > 0.5 && ring_r > ring_inner && ring_r < ring_outer) {
        float t   = (ring_r - ring_inner) / ring_span;   // 0..1 across the ring
        float rwt = ring_rw / ring_span;                 // pixel footprint in t-space

        // Concentric brightness bands from a few octaves. Each octave's contrast is faded out
        // once a pixel spans a large fraction of its period (analytic anti-aliasing), so the
        // rings read as smooth soft bands instead of a sparkling moire when small on screen.
        float bands = 0.0, amp = 0.5, w = 26.0;
        for (int bi = 0; bi < 4; ++bi) {
            float fade = saturate(1.0 - ring_rw * w * 0.5);
            bands += amp * fade * sin(ring_r * w + seed * float(bi + 1) * 1.7);
            amp *= 0.5; w *= 1.9;
        }
        // Fine dust grain, likewise faded out when it would alias.
        float grain_fade = saturate(1.0 - ring_rw * 60.0);
        bands += 0.18 * grain_fade * (fbm(float2(ring_r * 9.0, seed * 1.3)) - 0.5) * 2.0;
        float bright = saturate(0.5 + 0.5 * bands);

        // A few Cassini-style divisions (see-through dark gaps), pixel-footprint-broadened so they
        // stay artifact-free at any size.
        float gaps = ring_gap(t, 0.34, 0.030, 0.85, rwt)
                   * ring_gap(t, 0.58, 0.020, 0.65, rwt)
                   * ring_gap(t, 0.78, 0.015, 0.45, rwt);

        // Soft inner/outer falloff, never sharper than the pixel footprint (kills edge aliasing).
        float ew_in  = max(0.05, rwt * 1.5);
        float ew_out = max(0.10, rwt * 1.5);
        float edge   = smoothstep(0.0, ew_in, t) * (1.0 - smoothstep(1.0 - ew_out, 1.0, t));

        ring_cov = saturate(edge * gaps * (0.30 + 0.70 * bright));

        // Subtle warm/cool dust variation across the radius.
        float3 dust_cool = float3(0.74, 0.76, 0.80);
        float3 dust_warm = float3(0.87, 0.80, 0.67);
        float3 dust = lerp(dust_cool, dust_warm, saturate(0.5 + 0.5 * sin(ring_r * 3.0 + seed)));
        ring_rgb = dust * (0.45 + 0.55 * bright);

        // Planet shadow cast onto the ring -- a real sphere-shadow-on-plane so it responds to the
        // orbit (seasons): work in the true ring-plane coords rp (the ring is a circle there, the
        // planet centre sits in the plane), split the star direction into an in-plane part and an
        // elevation `star_elev` (sin of the star's angle above the ring plane, supplied per-frame
        // from the planet's orbital position). A ring point is shadowed when the ray toward the
        // star passes within one planet radius of the centre; the umbra REACH along the plane is
        // R/sin(elev), so it stretches into a long thin band near equinox (star in the plane) and
        // retracts toward the planet near solstice (star high above/below). Soft on both edges ->
        // one continuous swath (no hard onset line).
        float2 rp     = float2(sp.x, sp.y / ring_tilt);     // true ring-plane position
        float2 sd     = normalize(Ldir + float2(1e-4, 0.0));
        float2 dstar  = normalize(float2(sd.x, sd.y / ring_tilt)); // in-plane dir toward the star
        float  cosEl  = sqrt(saturate(1.0 - star_elev * star_elev));
        float  sdot   = dot(rp, dstar);                     // toward-star projection; shadow: sdot<0
        float  dist   = sqrt(max(dot(rp, rp) - cosEl * cosEl * sdot * sdot, 0.0));
        float  cyl    = 1.0 - smoothstep(0.80, 1.05, dist); // soft umbra cylinder (planet radius ~1)
        float  pen    = smoothstep(-0.28, 0.28, -sdot);     // soft penumbra at the terminator plane
        float  shadow = cyl * pen;
        ring_rgb *= (1.0 - 0.82 * shadow);
        ring_rgb *= starcol;                              // rings are star-lit dust
    }
    // FAR ring (behind the planet): drawn first; the opaque sphere below overwrites the overlap.
    if (ring_cov > 0.0 && !ring_near) {
        float ra = ring_cov * 0.9;
        out_rgb = ring_rgb * ra;
        out_a   = ra;
    }

    // ---- Atmosphere glow (BEHIND the sphere) ----
    // A soft halo centred on the limb, present on BOTH sides of r=1 and composited BEFORE the
    // sphere so the sphere's anti-aliased edge fades into this GLOW rather than into the black
    // background. Without it, premultiplied coverage drops to ~0.5 at r=1 with black showing
    // through (bg*(1-a)), while the halo only existed OUTSIDE the disc -> a thin dark trough all
    // the way around the limb (worst/blocky at the poles). Filling the gap removes the ring.
    if (has_atmo > 0.5) {
        float glow = exp(-abs(r - 1.0) * 7.0);                     // peak at the limb, both sides
        float3 gN  = float3(sp.x, sp.y, 0.0) / max(r, 1e-3);
        float3 gL  = normalize(float3(Ldir.x, Ldir.y, 0.15));
        float  glit = saturate(dot(gN, gL) * 0.5 + 0.5);           // brighter on the lit limb
        float  ga  = glow * (0.22 + 0.55 * glit);
        out_rgb = out_rgb * (1.0 - ga) + atmo_tint * starcol * ga; // premultiplied over (far ring)
        out_a   = ga + out_a * (1.0 - ga);
    }

    // ---- Sphere ----
    // Anti-aliased disc coverage: instead of a hard r<=1 cutoff (which stair-steps the limb),
    // feather the outermost pixel of the edge so the boundary is smooth at any zoom.
    float disc = 1.0 - smoothstep(1.0 - disc_aa, 1.0 + disc_aa, r);
    if (disc > 0.0) {
        float  nz = sqrt(saturate(1.0 - r2));
        float3 N  = float3(sp.x, sp.y, nz);
        // Light sits mostly in the screen plane with only a small camera-ward tilt, so the
        // day/night terminator sweeps across the visible disc as a broad gradient (not pinned to
        // the rim). The direction is physical (planet -> its star) so the shadow is on the correct
        // side; here we make it *look* soft and real.
        float3 L   = normalize(float3(Ldir.x, Ldir.y, 0.15));
        float  ndl = dot(N, L);
        float  day = smoothstep(-0.22, 0.24, ndl);          // wide, soft terminator
        float  wrap = saturate(ndl * 0.5 + 0.5);            // Lambert wrap (thick-atmosphere softness)
        day = lerp(day, wrap * wrap, 0.25);

        // Spherical UV (rotates about the vertical axis with the rotation phase).
        float lon = atan2(N.x, N.z) + rot;
        float lat = asin(clamp(N.y, -1.0, 1.0));
        float2 uv = float2(lon, lat) * 1.6;

        float em = 0.0;
        float3 alb = planet_albedo(ptype, uv, lat, gn, seed, em);

        // Clouds (ocean/terran): a domain-warped layer in the genome's cloud tint, scrolling
        // slowly, on the lit side.
        if (cloud_amt > 0.01) {
            float cl = smoothstep(0.52, 0.78, fbm(domain_warp(uv * 1.2 + float2(time * 0.015, seed * 2.0), 0.6)));
            alb = lerp(alb, g_cloud.xyz, saturate(cl * cloud_amt) * day);
        }

        // Day tinted by the star colour; near-black night; emissive (lava) bypasses lighting.
        float3 lit = alb * (day * starcol * 1.12 + 0.02);
        lit += alb * em;
        // Exotic-anomaly worlds carry a faint self-luminous shimmer so a rare mutation reads as
        // "special" even on the night side (a light structural flourish atop its exotic palette).
        if (gn.anomaly > 0.5) {
            float shim = fbm(uv * 3.2 + seed + time * 0.04);
            lit += alb * (0.06 + 0.12 * shim);
        }
        // A hair of terminator scatter for atmospheres (faint bluish glow where day meets night).
        if (has_atmo > 0.5) {
            float term = saturate(day * (1.0 - day) * 4.0);
            lit += float3(0.22, 0.38, 0.62) * starcol * term * 0.12;
        }
        // Limb darkening -> the disc edge darkens for roundness, but keeps a small floor so the
        // very limb never crushes to an opaque near-black rim. Without the floor, pow(nz,*) drives
        // lit->0 at r~1 while the coverage/alpha stays high, so the premultiplied edge paints an
        // opaque dark line over the background/halo (the "thin black boundary" artifact).
        lit *= (0.12 + 0.88 * pow(nz, 0.5));
        // Hazy atmospheric limb on the sphere surface (added AFTER limb darkening so it isn't
        // crushed). This is the on-surface scattering; the seam into space is handled by the
        // atmosphere glow drawn BEHIND the sphere, so the edge blends into the halo, not black.
        if (has_atmo > 0.5) {
            float limb = smoothstep(0.25, 1.0, 1.0 - nz);
            lit += atmo_tint * starcol * limb * (0.10 + 0.35 * day);
        }

        // Composite the sphere OVER whatever is behind it (far ring / atmosphere glow) using the
        // anti-aliased coverage, so the limb feathers into the glow instead of a dark ring.
        out_rgb = out_rgb * (1.0 - disc) + lit * disc;   // premultiplied "over"
        out_a   = disc + out_a * (1.0 - disc);
    }
    // NEAR ring (in front of the planet): composited over the sphere so it crosses the lower disc.
    if (ring_cov > 0.0 && ring_near) {
        float ra = ring_cov * 0.9;
        out_rgb = out_rgb * (1.0 - ra) + ring_rgb * ra;   // premultiplied "over"
        out_a   = ra + out_a * (1.0 - ra);
    }

    out_rgb *= vis;
    out_a   *= vis;
    return float4(out_rgb, out_a);   // premultiplied
}
