// sprite.vert.hlsl — Phase 3 sprite-batch vertex shader.
//
// SDL3 GPU resource-binding contract (DXIL/D3D12 path, see SDL_gpu.h):
//   vertex uniform buffers => register(b[n], space1).
// Vertex semantics start at TEXCOORD0 and increment (SDL maps non-system-value semantics to
// TEXCOORD for D3D12). The CPU-side vertex input layout in the graphics pipeline matches this
// order: location 0 = position, location 1 = uv, location 2 = color.
//
// Each sprite is pre-transformed CPU-side into 4 world-space corner vertices by the batcher,
// so the vertex shader only applies the camera view-proj — no per-sprite model matrix here.

cbuffer UBO : register(b0, space1)
{
    float4x4 view_proj; // column-major; engine uploads Mat4 directly (data[col*4+row]).
};

struct VSInput
{
    float2 position : TEXCOORD0; // 2D world-space position (already transformed)
    float2 uv       : TEXCOORD1; // atlas UV
    float4 color    : TEXCOORD2; // per-vertex RGBA tint
    float4 custom   : TEXCOORD3; // per-vertex custom shader parameters
};

struct VSOutput
{
    float4 position  : SV_Position;
    float2 uv        : TEXCOORD0;
    float4 color     : TEXCOORD1;
    float2 world_pos : TEXCOORD2; // world-space position, interpolated for per-pixel 2D lighting
    float4 custom    : TEXCOORD3; // per-vertex custom shader parameters
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.position  = mul(view_proj, float4(input.position, 0.0f, 1.0f));
    output.uv        = input.uv;
    output.color     = input.color;
    output.world_pos = input.position;
    output.custom    = input.custom;
    return output;
}
