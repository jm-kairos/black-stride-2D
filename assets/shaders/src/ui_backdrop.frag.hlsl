// ui_backdrop.frag.hlsl — frosted-glass UI backdrop pixel shader.
//
// Renders a real backdrop-blur behind RmlUi panels. The engine produces a half-res, gaussian-blurred
// copy of the pre-UI scene into ui_backdrop_rt each frame; this shader samples it in SCREEN space and
// tints it, so a panel reads as tinted frosted glass that tracks the scene as the window is dragged.
//
// Vertex stage: rml.vert (shared with the normal UI pipeline). It emits SV_Position (pixel coords,
// top-left origin), the premultiplied element colour, and the element-local uv (unused here).
//
// Screen-space uv comes from SV_Position.xy (the rasterizer's pixel coordinate) times 1/screen_size,
// so no per-move geometry regeneration is needed — the fragment always samples the correct scene
// pixel behind it. Output is premultiplied-alpha for the RmlUi (ONE, ONE_MINUS_SRC_ALPHA) blend.

Texture2D    bg_tex : register(t0, space2);
SamplerState bg_smp : register(s0, space2);

cbuffer FrostParams : register(b0, space3)
{
    float4 inv_screen; // xy = 1/swap_width, 1/swap_height; zw unused
    float4 tint;       // rgb = tint colour, a = tint strength [0..1] (lerp scene -> tint)
};

struct PSInput
{
    float4 position : SV_Position; // pixel coordinates (top-left origin)
    float4 color    : TEXCOORD0;   // premultiplied element colour; .a = element opacity
    float2 uv       : TEXCOORD1;   // element-local uv (unused)
};

float4 main(PSInput input) : SV_Target0
{
    float2 suv   = input.position.xy * inv_screen.xy; // [0,1] screen uv
    float3 scene = bg_tex.Sample(bg_smp, suv).rgb;    // blurred scene behind the panel
    float3 rgb   = lerp(scene, tint.rgb, tint.a);     // tinted frosted glass
    float  a     = input.color.a;                     // element / decorator opacity
    return float4(rgb * a, a);                         // premultiplied-alpha over
}
