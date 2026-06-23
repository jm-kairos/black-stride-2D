// starfield_lod.frag.hlsl — procedural starfield for lowest zoom (galaxy view)
// Drawn fullscreen as a post-process. No texture sampling; stars are hashed
// from world-space grid cells so they never flicker during camera movement.
//
// Uniform layout (b0, space3):
//   params0 : camera_position_x, camera_position_y, zoom, fb_width
//   params1 : fb_height, cell_size, time, seed

struct PSInput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

cbuffer StarfieldLodParams : register(b0, space3)
{
    float4 params0; // cam_x, cam_y, zoom, fb_w
    float4 params1; // fb_h, density, size_mul, brightness_mul
    float4 params2; // seed, 0, 0, 0
};

// Fast hash (float3 variant).
float hash1(float n)
{
    return frac(sin(n * 127.1) * 43758.5453);
}

float hash2(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float2 hash22(float2 p, float seed)
{
    float3 p3 = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    float2 h = frac((p3.xx + p3.yz) * p3.zy);
    h.x += seed * 0.6180339887;
    h = frac(h);
    return h;
}

float3 hash_color(float2 p, float seed)
{
    float h = hash2(p + float2(seed, seed * 0.37));
    // Map 0..1 to blue-white for bright stars, orange-red for dim.
    float t = h;
    if (t < 0.33)      return float3(0.6, 0.7, 1.0); // blue-white
    else if (t < 0.66) return float3(1.0, 0.95, 0.9); // white
    else               return float3(1.0, 0.75, 0.5); // warm
}

float4 main(PSInput input) : SV_Target0
{
    float2 uv = input.uv;
    float cam_x = params0.x;
    float cam_y = params0.y;
    float zoom  = params0.z;
    float fb_w  = params0.w;
    float fb_h  = params1.x;
    float density = params1.y;
    float size_mul = params1.z;
    float brightness_mul = params1.w;
    // The raw seed can be a large u32 like 0xDEADBEEF. Reduce it to a small
    // fractional value so all hash operations keep float precision and give
    // the same scattered star-dot pattern as the old LOD shader.
    float seed = frac(params2.x * 0.1031);

    // Convert UV to world position.
    float2 world_pos = float2(cam_x, cam_y) + (uv - 0.5) * float2(fb_w, fb_h) / zoom;

    // Wrap world position into the repeating field (4096 units).
    float field_size = 4096.0;
    float2 wrap_pos = fmod(world_pos, field_size);
    if (wrap_pos.x < 0.0) wrap_pos.x += field_size;
    if (wrap_pos.y < 0.0) wrap_pos.y += field_size;

    // Determine cell grid coordinate.
    float cell_size = 64.0;
    int2 cell = int2(floor(wrap_pos / cell_size));
    float2 cell_frac = frac(wrap_pos / cell_size);

    float3 accum = float3(0.0, 0.0, 0.0);
    float  accum_a = 0.0;

    // Search neighbouring cells for nearby stars.
    for (int dy = -2; dy <= 2; ++dy)
    {
        for (int dx = -2; dx <= 2; ++dx)
        {
            int2 neighbor = cell + int2(dx, dy);
            // Wrap cell indices for tiling.
            int2 wrapped_cell = int2(
                (neighbor.x % 64 + 64) % 64,
                (neighbor.y % 64 + 64) % 64
            );

            float2 cell_id = float2(wrapped_cell);

            // Hash cell to determine if it contains a star.
            float presence = hash2(cell_id + float2(seed * 100.0, seed * 200.0));
            if (presence > density)
                continue;

            // Hash star offset within cell.
            float2 star_offset = hash22(cell_id, seed);
            float2 star_pos = (cell_id + star_offset) * 64.0;

            // Handle wrapping for neighbours across field boundary.
            float2 delta = star_pos - wrap_pos;
            if (delta.x >  field_size * 0.5) delta.x -= field_size;
            if (delta.x < -field_size * 0.5) delta.x += field_size;
            if (delta.y >  field_size * 0.5) delta.y -= field_size;
            if (delta.y < -field_size * 0.5) delta.y += field_size;

            float dist_sq = dot(delta, delta);
            if (dist_sq > 160.0 * 160.0)
                continue;

            // Star size: 16-40 world units base, scaled by size_mul.
            float star_size = (16.0 + hash1(cell_id.x * 31.0 + cell_id.y * 57.0 + seed) * 24.0)
                              * size_mul;

            // Brightness: heavily skewed — most stars very dim, a few medium.
            float b_hash = hash1(cell_id.x * 13.0 + cell_id.y * 97.0 + seed * 0.5);
            float brightness = (0.05 + pow(b_hash, 3.0) * 0.35) * brightness_mul;

            // Soft Gaussian falloff.
            float gauss = exp(-dist_sq / (3.0 * star_size * star_size));
            float intensity = gauss * brightness;

            float3 col = hash_color(cell_id, seed);
            accum += col * intensity;
            accum_a += intensity;
        }
    }

    // Clamp and gamma-correct slightly.
    accum = saturate(accum);
    accum_a = saturate(accum_a);

    return float4(accum * 0.8, accum_a);
}
