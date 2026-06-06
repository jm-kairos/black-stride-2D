// quad.vert.hlsl — Phase 2 first-quad vertex shader.
//
// SDL3 GPU resource-binding contract (DXIL/D3D12 path, see SDL_gpu.h):
//   vertex uniform buffers => register(b[n], space1).
// Vertex semantics must start at TEXCOORD0 and increment (SDL maps non-system-value
// semantics to TEXCOORD for D3D12). The vertex input layout is declared CPU-side in the
// graphics pipeline (location 0 = position, location 1 = color), matching this order.

cbuffer UBO : register(b0, space1)
{
    float4x4 view_proj; // column-major; engine uploads Mat4 directly (data[col*4+row]).
};

struct VSInput
{
    float2 position : TEXCOORD0; // 2D world-space position
    float4 color    : TEXCOORD1; // per-vertex RGBA
};

struct VSOutput
{
    float4 position : SV_Position;
    float4 color    : TEXCOORD0;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.position = mul(view_proj, float4(input.position, 0.0f, 1.0f));
    output.color    = input.color;
    return output;
}
