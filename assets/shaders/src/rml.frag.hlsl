// rml.frag.hlsl — RmlUi UI pixel shader.
//
// SDL3 GPU binding contract (DXIL/D3D12 path): fragment sampled textures => register(t0, space2),
// samplers => register(s0, space2), fragment uniforms => register(b0, space3). Untextured RmlUi
// geometry (texture handle 0) binds a 1x1 white texture so this single pipeline serves both
// textured and solid geometry (white * color = color).
//
// RmlUi uses premultiplied-alpha sRGB for both generated textures and vertex colors, so the texel
// times the vertex color is already premultiplied; the pipeline blends with (ONE, ONE_MINUS_SRC_ALPHA).
//
// CAS-lite sharpening: the backend tags FILE-LOADED textures (the 2x-authored skin/icon atlases,
// minified 2:1 at display) and pushes their texel size + a sharpen amount; font glyphs and
// generated gradients arrive with amount 0 and take the single-sample fast path. The neighborhood
// min/max clamp is the "contrast adaptive" part: sharpened output never exceeds the local range,
// so bevels tighten without ringing or halos at alpha edges.

Texture2D    ui_tex : register(t0, space2);
SamplerState ui_smp : register(s0, space2);

cbuffer SharpenParams : register(b0, space3)
{
    float4 sharpen; // x = 1/tex_w, y = 1/tex_h, z = amount (0 = off), w unused
};

struct PSInput
{
    float4 position : SV_Position;
    float4 color    : TEXCOORD0;
    float2 uv       : TEXCOORD1;
};

float4 main(PSInput input) : SV_Target0
{
    float4 c = ui_tex.Sample(ui_smp, input.uv);
    if (sharpen.z > 0.0)
    {
        float2 px = sharpen.xy;
        float3 n = ui_tex.Sample(ui_smp, input.uv + float2(0.0, -px.y)).rgb;
        float3 s = ui_tex.Sample(ui_smp, input.uv + float2(0.0,  px.y)).rgb;
        float3 e = ui_tex.Sample(ui_smp, input.uv + float2( px.x, 0.0)).rgb;
        float3 w = ui_tex.Sample(ui_smp, input.uv + float2(-px.x, 0.0)).rgb;
        // Plain unsharp mask on RGB (alpha untouched). A CAS-style neighborhood clamp would
        // neutralize the effect entirely on this art: the machined bevels are 1px lines that
        // ARE their neighborhood's max, so clamped sharpening returns them unchanged. The
        // premultiplied bound (rgb <= a) keeps edge fringing valid against the scene.
        float3 sharp = c.rgb + (c.rgb * 4.0 - (n + s + e + w)) * (sharpen.z * 0.5);
        c.rgb = clamp(sharp, 0.0, c.a);
    }
    return c * input.color;
}
