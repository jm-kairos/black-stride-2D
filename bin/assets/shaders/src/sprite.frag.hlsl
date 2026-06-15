// sprite.frag.hlsl — Phase 3 sprite-batch pixel shader.
//
// SDL3 GPU binding contract (DXIL/D3D12 path): fragment sampled textures => register(t[n], space2),
// samplers => register(s[n], space2). These are bound together via SDL_BindGPUFragmentSamplers.
// The pipeline's fragment shader must declare num_samplers = 2 to match.
//
// Samples the bound texture and multiplies by the interpolated per-vertex tint. A solid-white
// 1x1 texture lets untextured/solid sprites reuse this same pipeline (white * tint = tint).

Texture2D    sprite_tex : register(t0, space2);
SamplerState sprite_smp : register(s0, space2);

// Occlusion atlas: tile opacity grids for all ships (R8_UNORM).
Texture2D    occ_tex : register(t1, space2);
SamplerState occ_smp : register(s1, space2);

// Movable 2D point lights (SDL3 GPU contract: fragment uniform buffers => register(b[n], space3)).
// Pushed per draw-run by the backend (SDL_PushGPUFragmentUniformData). The game owns an editable
// list of lights; the backend packs up to BS_MAX_LIGHTS of them here and accumulates their
// contribution over an ambient floor. `light_params.y` is the per-run lit flag: 0 => fullbright,
// which is how UI/HUD layers (and a scene with zero lights) stay unlit.
// MUST be kept in sync with gpu_lights / BS_BACKEND_MAX_LIGHTS in renderer_backend_sdlgpu.cpp.
#define BS_MAX_LIGHTS 16

cbuffer LightUBO : register(b0, space3)
{
    float4 light_params;                    // x = active light count, y = lit flag (0 = fullbright)
    float4 ambient_color;                   // rgb = ambient light floor, w = unused
    float4 light_pos_radius[BS_MAX_LIGHTS]; // xy = world position, z = radius, w = intensity
    float4 light_color[BS_MAX_LIGHTS];      // rgb = light color, w = per-light enabled (0 = off)
};

#define BS_MAX_TILE_OCCLUDERS 4

cbuffer OcclusionUBO : register(b1, space3)
{
    float4 occ_params; // x = active occluder count
    float4 occluder_data[BS_MAX_TILE_OCCLUDERS * 3];
    // Per occluder (3 float4s):
    //   [i*3+0]: xy = origin_world, z = cos(angle), w = sin(angle)
    //   [i*3+1]: x = tile_size, y = half_w, z = half_h, w = unused
    //   [i*3+2]: xy = atlas_uv_offset, zw = atlas_uv_scale (1.0 / atlas_size)
};

struct PSInput
{
    float4 position  : SV_Position;
    float2 uv        : TEXCOORD0;
    float4 color     : TEXCOORD1;
    float2 world_pos : TEXCOORD2; // interpolated world-space position from the vertex shader
};

// 2D grid traversal (Amanatides & Woo) through a tile grid.
// Returns 1.0 if the ray from light_pos to pixel_pos is unblocked, 0.0 if blocked.
float occlusion_for_occluder(float2 light_pos, float2 pixel_pos, int occ_idx)
{
    float4 s0 = occluder_data[occ_idx * 3 + 0];
    float4 s1 = occluder_data[occ_idx * 3 + 1];
    float4 s2 = occluder_data[occ_idx * 3 + 2];

    float2 origin = s0.xy;
    float  ca = s0.z;  float sa = s0.w;
    float  ts = s1.x;
    float  hw = s1.y;  float hh = s1.z;
    float2 atlas_off = s2.xy;
    float2 atlas_scale = s2.zw;

    // Transform both points to local space (inverse pose).
    float2 lp = float2(
        (light_pos.x - origin.x) * ca + (light_pos.y - origin.y) * sa,
       -(light_pos.x - origin.x) * sa + (light_pos.y - origin.y) * ca);
    float2 pp = float2(
        (pixel_pos.x - origin.x) * ca + (pixel_pos.y - origin.y) * sa,
       -(pixel_pos.x - origin.x) * sa + (pixel_pos.y - origin.y) * ca);

    // To tile indices (same math as local_to_tile).
    float2 g0 = float2(lp.x + hw, hh - lp.y);
    float2 g1 = float2(pp.x + hw, hh - pp.y);
    int2 c0 = int2(floor(g0 / ts));
    int2 c1 = int2(floor(g1 / ts));

    // DDA setup.
    int2 step = int2(sign(c1 - c0));
    float2 t_delta = abs(float2(1.0, 1.0) / (c1 - c0 + 0.0001));
    float2 t_max = float2(
        step.x > 0 ? (1.0 - frac(g0.x / ts)) * t_delta.x : frac(g0.x / ts) * t_delta.x,
        step.y > 0 ? (1.0 - frac(g0.y / ts)) * t_delta.y : frac(g0.y / ts) * t_delta.y);

    int2 cur = c0;
    int max_steps = abs(c1.x - c0.x) + abs(c1.y - c0.y);
    for (int s = 0; s < max_steps; ++s)
    {
        if (cur.x == c1.x && cur.y == c1.y) break;
        if (t_max.x < t_max.y) { cur.x += step.x; t_max.x += t_delta.x; }
        else                   { cur.y += step.y; t_max.y += t_delta.y; }

        // Sample atlas. Atlas UV for tile (col,row) = atlas_off + (col,row) * atlas_scale.
        float2 uv = atlas_off + float2(cur.x, cur.y) * atlas_scale;
        float occ = occ_tex.SampleLevel(occ_smp, uv, 0).r;
        if (occ > 0.5) return 0.0; // blocked
    }
    return 1.0; // unblocked
}

float4 main(PSInput input) : SV_Target0
{
    float4 texel = sprite_tex.Sample(sprite_smp, input.uv) * input.color;

    float3 lighting = float3(1.0f, 1.0f, 1.0f); // default: fullbright (unlit run / no lights)
    if (light_params.y > 0.5f)
    {
        float3 acc   = ambient_color.rgb;
        int    count = (int)light_params.x;
        int    occ_count = (int)occ_params.x;
        [loop]
        for (int i = 0; i < count; ++i)
        {
            if (light_color[i].w < 0.5f) continue; // per-light disabled

            // Smooth radial falloff: linear distance ramp, squared for a soft hot core.
            float dist  = length(input.world_pos - light_pos_radius[i].xy);
            float atten = saturate(1.0f - dist / max(light_pos_radius[i].z, 0.0001f));
            atten *= atten;

            float occ = 1.0;
            if (occ_count > 0)
            {
                [loop]
                for (int s = 0; s < occ_count; ++s)
                {
                    occ *= occlusion_for_occluder(light_pos_radius[i].xy, input.world_pos, s);
                    if (occ < 0.01) break; // fully shadowed
                }
            }
            acc += light_color[i].rgb * (atten * light_pos_radius[i].w * occ);
        }
        lighting = acc;
    }

    return float4(texel.rgb * lighting, texel.a);
}
