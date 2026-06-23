// starfield.frag.hlsl — Gaussian glow + diffraction spikes for per-star quads
// Blends additively against a black background.

struct PSInput
{
    float4 position : SV_Position;
    float2 coord    : TEXCOORD0;
    float  alpha    : TEXCOORD1;
    float3 color    : TEXCOORD2;
};

float4 main(PSInput input) : SV_Target0
{
    // Approximate radial distance from the star center using the interpolated coord.
    // coord is a unit vector at the corners → length varies 0..1 across the quad.
    float d = length(input.coord);

    // Gaussian-like radial glow. exp2(-8*d^2) gives a soft natural falloff.
    float glow = exp2(-8.0 * d * d);

    // Diffraction spikes: thin bright cross along the axes.
    // Only strong for brighter stars (input.alpha > 0.4).
    float spike = 0.0;
    if (input.alpha > 0.3)
    {
        float sx = max(0.0, 1.0 - 20.0 * abs(input.coord.x));
        float sy = max(0.0, 1.0 - 20.0 * abs(input.coord.y));
        spike = (sx + sy) * 0.15 * input.alpha;
    }

    float a = input.alpha * (glow + spike);
    // Additive blending: color carries the intensity.
    return float4(input.color * a, 1.0);
}
