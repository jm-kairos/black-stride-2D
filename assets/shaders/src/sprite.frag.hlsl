// sprite.frag.hlsl — sprite-batch pixel shader.
//
// SDL3 GPU binding contract (DXIL/D3D12 path): fragment sampled textures => register(t[n], space2),
// samplers => register(s[n], space2). These are bound together via SDL_BindGPUFragmentSamplers.
//
// Samples the bound texture and multiplies by the interpolated per-vertex tint. A solid-white
// 1x1 texture lets untextured/solid sprites reuse this same pipeline (white * tint = tint).

Texture2D    sprite_tex  : register(t0, space2);
SamplerState sprite_smp  : register(s0, space2);

// Movable 2D point lights (SDL3 GPU contract: fragment uniform buffers => register(b[n], space3)).
// Pushed per draw-run by the backend. MUST be kept in sync with gpu_lights in renderer_backend_sdlgpu.cpp.
#define BS_MAX_LIGHTS 16

cbuffer LightUBO : register(b0, space3)
{
    float4 light_params;                    // x = active light count, y = lit flag (0 = fullbright)
    float4 ambient_color;                   // rgb = ambient light floor, w = unused
    float4 light_pos_radius[BS_MAX_LIGHTS]; // xy = world position, z = radius, w = intensity
    float4 light_color[BS_MAX_LIGHTS];      // rgb = light color, w = per-light enabled (0 = off)
    float4 glow[8];                         // tunable glow/heat params (see renderer_types.h)
};

struct PSInput
{
    float4 position  : SV_Position;
    float2 uv        : TEXCOORD0;
    float4 color     : TEXCOORD1;
    float2 world_pos : TEXCOORD2;
    float4 custom    : TEXCOORD3;
};

float4 main(PSInput input) : SV_Target0
{
    // Unpack glow parameters from the uniform block
    float g_intensity    = glow[0].x;
    float g_falloff      = glow[0].y;
    float g_head_mult    = glow[0].z;
    float g_head_falloff = glow[0].w;
    float g_head_range   = glow[1].x;
    float g_distort_amp  = glow[1].y;
    float g_wave_speed   = glow[1].z;
    float g_wave_freq    = glow[1].w;
    float g_jitter_speed = glow[2].x;
    float g_jitter_freq  = glow[2].y;
    float3 g_glow_tint   = glow[3].rgb;
    float3 g_temp_cool   = glow[4].rgb;
    float3 g_temp_warm   = glow[5].rgb;
    float3 g_temp_hot    = glow[6].rgb;

    // ---- 1. Heat distortion: warp UV with a travelling sine wave ------------------------
    float2 uv = input.uv;
    float wave1 = sin(input.custom.y * g_wave_speed + uv.y * g_wave_freq);
    float wave2 = sin(input.custom.y * g_jitter_speed + uv.y * g_jitter_freq) * 0.3;
    uv.x += (wave1 + wave2) * g_distort_amp * input.custom.x;
    float4 texel = sprite_tex.Sample(sprite_smp, uv) * input.color;

    // ---- 2. Colour temperature gradient (white-hot head → red tail) ----------------------
    float t = uv.y; // 0 = tail, 1 = head
    float3 temp = lerp(g_temp_cool, g_temp_warm, smoothstep(0.0, 0.4, t));
    temp = lerp(temp, g_temp_hot, smoothstep(0.6, 1.0, t));
    texel.rgb *= lerp(float3(1.0, 1.0, 1.0), temp, input.custom.x);

    // ---- 3. Head bloom: extra intense glow at the head ---------------------------------
    float2 head_uv = float2(uv.x, 1.0);
    float head_dist_sq = dot(uv - head_uv, uv - head_uv);
    float head_factor = smoothstep(g_head_range, 1.0, uv.y);
    float head_bloom = exp(-head_dist_sq * g_head_falloff) * head_factor * g_head_mult * input.custom.x;
    texel.rgb += float3(1.0, 0.95, 0.85) * head_bloom;

    // ---- Base procedural glow (radial, all sprites) ---------------------------------------
    float2 uv_center = uv - float2(0.5, 0.5);
    float dist_sq = dot(uv_center, uv_center);
    float glow_val = exp(-dist_sq * g_falloff) * g_intensity * input.custom.x;
    texel.rgb += g_glow_tint * glow_val;

    float3 lighting = float3(1.0f, 1.0f, 1.0f);
    float3 volumetric = float3(0.0f, 0.0f, 0.0f);
    if (light_params.y > 0.5f)
    {
        float3 acc   = ambient_color.rgb;
        int    count = (int)light_params.x;
        [loop]
        for (int i = 0; i < count; ++i)
        {
            if (light_color[i].w < 0.5f) continue;

            float2 to_light = input.world_pos - light_pos_radius[i].xy;
            float dist      = length(to_light);
            float radius    = max(light_pos_radius[i].z, 0.0001f);

            // Direct light: sharp quadratic falloff within radius
            float direct = saturate(1.0f - dist / radius);
            direct *= direct;

            // Volumetric glow: wide exponential falloff (extends ~2.5x radius)
            float vol = exp(-dist / (radius * 0.4f));

            acc += light_color[i].rgb * (direct * light_pos_radius[i].w);
            volumetric += light_color[i].rgb * (vol * light_pos_radius[i].w * 0.5f);
        }
        lighting = acc;
    }

    // Volumetric glow is added on top (unmodulated by sprite alpha) so empty pixels pick it up too
    return float4(texel.rgb * lighting + volumetric * texel.a, texel.a);
}
