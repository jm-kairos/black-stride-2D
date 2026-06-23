// hud_overlay.frag.hlsl — post-composite HUD overlay: vignette + scanlines
// Reads the already-composited swapchain image and applies a subtle screen-space effect.

Texture2D    src_tex : register(t0, space2);
SamplerState src_smp : register(s0, space2);

cbuffer HudOverlayParams : register(b0, space3)
{
    float4 params; // x = vignette intensity, y = scanline intensity, z = scanline density, w = time
};

struct PSInput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target0
{
    float2 uv = input.uv;
    float4 color = src_tex.Sample(src_smp, uv);

    // Vignette: darken corners based on radial distance from center.
    float2 centered = uv - 0.5;
    float vignette_falloff = dot(centered, centered) * 2.0; // 0 at center, ~1 at corners
    vignette_falloff = saturate(vignette_falloff);
    vignette_falloff = pow(vignette_falloff, 1.6);
    color.rgb *= lerp(1.0, 1.0 - params.x, vignette_falloff);

    // Scanlines: subtle horizontal lines animated by time.
    float scan = sin((uv.y * params.z * 3.14159265) + (params.w * 0.5));
    scan = (scan + 1.0) * 0.5; // 0..1
    float scan_mul = lerp(1.0 - params.y, 1.0, scan);
    color.rgb *= scan_mul;

    return color;
}
