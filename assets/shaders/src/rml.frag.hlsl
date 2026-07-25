// rml.frag.hlsl — RmlUi UI pixel shader.
//
// SDL3 GPU binding contract (DXIL/D3D12 path): fragment sampled textures => register(t0, space2),
// samplers => register(s0, space2). Untextured RmlUi geometry (texture handle 0) binds a 1x1 white
// texture so this single pipeline serves both textured and solid geometry (white * color = color).
//
// RmlUi uses premultiplied-alpha sRGB for both generated textures and vertex colors, so the texel
// times the vertex color is already premultiplied; the pipeline blends with (ONE, ONE_MINUS_SRC_ALPHA).

Texture2D    ui_tex : register(t0, space2);
SamplerState ui_smp : register(s0, space2);

struct PSInput
{
    float4 position : SV_Position;
    float4 color    : TEXCOORD0;
    float2 uv       : TEXCOORD1;
};

float4 main(PSInput input) : SV_Target0
{
    return ui_tex.Sample(ui_smp, input.uv) * input.color;
}
