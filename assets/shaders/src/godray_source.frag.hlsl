// godray_source.frag.hlsl — god-ray occlusion-buffer source: the sun as a bright disc + halo.
// Drawn fullscreen into the (half-res) occlusion target BEFORE the hull silhouettes darken it;
// godray_blur then smears whatever survives radially away from the sun.

cbuffer GodraySource : register(b0, space3)
{
    float4 sun;  // x,y = sun centre in screen px (top-left origin), z = disc radius px, w = halo scale (in radii)
    float4 tint; // rgb = shaft colour, w = source gain
    float4 fb;   // x,y = framebuffer size px
};

struct PSInput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target0
{
    float2 px   = input.uv * fb.xy;
    float  dist = length(px - sun.xy);
    // Bright core with a soft rim, plus an exponential halo so the shafts have body past the
    // disc edge instead of collapsing to a hard circle.
    float core = 1.0 - smoothstep(sun.z * 0.6, sun.z, dist);
    float halo = 0.35 * exp(-max(dist - sun.z, 0.0) / max(sun.z * sun.w, 1.0));
    float b    = saturate(core + halo) * tint.w;
    return float4(tint.rgb * b, 1.0);
}
