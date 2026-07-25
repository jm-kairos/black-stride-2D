// star_surface.frag.hlsl — real-time procedural Sun-like star surface.
//
// Renders an animated photosphere onto a screen-space quad: convective granulation (domain-
// warped fbm), bright emissive hotspots, dark sunspots, limb darkening, a limb-brightening rim,
// and a soft outer corona. Output is PREMULTIPLIED alpha for the premult-over pipeline
// (out = src + dst*(1-src.a)) so the opaque disc occludes the nebula while the corona glows.

cbuffer StarSurfaceParams : register(b0, space3)
{
    float4 a; // xy = centre px, z = body(disc) radius px, w = outer glow radius px
    float4 b; // xyz = star colour tint, w = elapsed time
    float4 c; // x = noise_scale, y = flow_speed, z = granule_contrast, w = visibility
    float4 d; // xy = framebuffer size px, z = hotspot_gain, w = sunspot_density
    float4 e; // x = limb_darkening, y = brightness, z = corona_strength (rim), w = dark_radius
    float4 f; // reserved
};

struct PSInput
{
    float4 position  : SV_Position;
    float2 local_pos : TEXCOORD0;
};

// --- hash / value-noise / fbm (same family as sunburst.frag / nebula.frag) --------------
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
    float val = 0.0, amp = 0.5, freq = 1.0;
    for (int i = 0; i < 4; ++i)
    {
        val += amp * vnoise(p * freq);
        amp  *= 0.5;
        freq *= 2.03;
    }
    return val; // ~0..1
}

float4 main(PSInput input) : SV_Target0
{
    float2 lp   = input.local_pos;      // -1..1 across outer glow radius
    float  dist = length(lp);           // 0 at centre, 1 at quad edge

    float body_r = a.z;
    float glow_r = a.w;
    float time   = b.w;
    float3 star_col = b.xyz;

    float noise_scale     = c.x;
    float flow_speed      = c.y;
    float granule_contrast= c.z;
    float visibility      = c.w;
    float hotspot_gain    = d.z;
    float sunspot_density = d.w;
    float limb_darkening  = max(e.x, 0.05);
    float brightness      = e.y;
    float corona_strength = e.z;
    float dark_radius     = max(e.w, 1.05); // radius (in body radii) of the black surround

    // Disc occupies the inner fraction body_r/glow_r of the quad. rr is 0..1 across the disc.
    float body_norm = max(body_r / max(glow_r, 1.0), 0.02);
    float rr = dist / body_norm;
    float t  = time * flow_speed;

    // Star hue (normalized so tinting adds colour without dimming).
    float3 hue = star_col / max(max(star_col.r, max(star_col.g, star_col.b)), 1e-3);

    // ---------------------------------------------------------------------------------------
    // CORONA + LIGHT RAYS — computed for every pixel (also "behind" the disc). The opaque disc
    // is composited OVER this below, so as the disc's soft limb fades to alpha 0 the corona
    // shows through continuously: no dark ring at the seam.
    // ---------------------------------------------------------------------------------------
    // Radial distance beyond the limb in BODY-RADIUS units (rr is already in those units).
    float rb = max(rr - 1.0, 0.0);

    // --- Thin chromosphere / corona rim hugging the limb, with spicule streaks from hotspots. ---
    float2 dir   = lp / max(dist, 1e-4);
    float2 sray  = dir * noise_scale;                         // angle-only limb sample
    float2 warpR = float2(fbm(sray * 0.5 + t * 0.3),
                          fbm(sray * 0.5 + 7.3 - t * 0.27));
    float  granR = fbm(sray + (warpR - 0.5) * 1.6 + t);
    granR = saturate(0.5 + (granR - 0.5) * granule_contrast);
    float  hotR  = smoothstep(0.55, 0.90, granR);             // where the limb is bright

    float  rim  = exp(-rb * 10.0);                            // very thin bright rim
    float  spic = hotR * hotspot_gain * exp(-rb * 4.0);       // spicule streaks off the limb
    float  emis = (rim + spic * 0.7) * corona_strength;       // total rim emission

    float3 rim_col    = lerp(float3(1.0, 0.45, 0.12), hue, 0.30);
    float3 corona_rgb = rim_col * emis;                       // premultiplied (rim colour)

    // --- Dark surround: OPAQUE BLACK that occludes the background out to `dark_radius`, then
    //     fades back to the scene BEYOND that radius. The whole interior (rr <= dark_radius) stays
    //     fully opaque so the nebula never bleeds through near the star; only the outer edge
    //     softens. Zoomed in, that outer edge is off-screen so the screen is solid black.
    //     Keyed to rr (radial) so the black region is a clean circle, not a square. ---
    float fade_band = max(dark_radius * 0.60, 0.5);
    float dark = 1.0 - smoothstep(dark_radius - fade_band, dark_radius, rr);
    float corona_a = saturate(emis + dark);                  // black around the star, rim on top

    // ---------------------------------------------------------------------------------------
    // DISC PHOTOSPHERE
    // ---------------------------------------------------------------------------------------
    float3 disc_rgb = float3(0.0, 0.0, 0.0);
    float  disc_a   = 0.0;

    if (rr <= 1.0)
    {
        // --- Sphere normal (z of the unit hemisphere) ---
        float mu = sqrt(max(0.0, 1.0 - rr * rr)); // 1 centre -> 0 limb

        // --- Surface sample coords ---
        float2 sp = lp / body_norm;             // -1..1 across the disc
        sp *= noise_scale;

        // --- Domain-warped granulation fbm (organic convection cells, no grid) ---
        float2 warp = float2(fbm(sp * 0.5 + t * 0.3),
                             fbm(sp * 0.5 + 7.3 - t * 0.27));
        float gran = fbm(sp + (warp - 0.5) * 1.6 + t);
        gran = saturate(0.5 + (gran - 0.5) * granule_contrast);

        // --- Emissive hotspots where granulation peaks (light bursting from the surface) ---
        float hot = smoothstep(0.60, 0.92, gran) * hotspot_gain;

        // --- Sunspots: dark cells from a slow low-frequency field ---
        float spotn = fbm(sp * 0.35 + 21.7 + t * 0.15);
        float spot_thresh = 1.0 - sunspot_density * 0.6;
        float spot = 1.0 - 0.75 * smoothstep(spot_thresh, spot_thresh + 0.10, spotn);

        // --- Base surface brightness ---
        float surf = (0.45 + 0.55 * gran) * spot + hot;

        // --- Limb darkening (moderate floor so the limb blends into the corona, no hard ring) ---
        float limb = pow(mu, limb_darkening);
        surf *= (0.35 + 0.65 * limb);
        surf *= brightness;

        // --- Colour: warm blackbody gradient tinted by the star's HUE (keeps saturation, ---
        //     avoids washing to pure white). Core is warm yellow-white, limb deep orange. ---
        float edge_t = 1.0 - mu;
        float3 core = float3(1.00, 0.83, 0.50);
        float3 edge = float3(0.98, 0.45, 0.16);
        float3 col  = lerp(core, edge, edge_t);
        col *= lerp(float3(1.0, 1.0, 1.0), hue, 0.6);              // apply hue without dimming
        col  = lerp(col, float3(1.0, 0.92, 0.78), saturate(hot) * 0.5); // hotspots warm-white

        // Wide soft limb: the disc melts into the corona instead of ending on a hard circle.
        float aa = smoothstep(1.0, 0.88, rr);
        disc_rgb = col * surf * aa; // premultiply
        disc_a   = aa;
    }

    // ---------------------------------------------------------------------------------------
    // Composite disc OVER corona (both premultiplied): out = disc + corona*(1 - disc.a).
    // ---------------------------------------------------------------------------------------
    float3 rgb   = disc_rgb + corona_rgb * (1.0 - disc_a);
    float  alpha = disc_a   + corona_a   * (1.0 - disc_a);

    // Visibility fade (sensor range, etc.) — keep premultiplied.
    rgb   *= visibility;
    alpha *= visibility;

    return float4(rgb, alpha);
}
