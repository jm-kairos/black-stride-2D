// bloom_composite.frag.hlsl — composite scene + bloom + aux streak back to swapchain

Texture2D    scene_tex     : register(t0, space2);
Texture2D    bloom_tex     : register(t1, space2);
Texture2D    aux_streak_tex : register(t2, space2);
Texture2D    aux_flare_tex : register(t3, space2);
SamplerState smp           : register(s0, space2);

cbuffer BloomParams : register(b0, space3)
{
    float4 params; // x = bloom intensity, y = aux streak intensity, z = flare intensity,
                   // w = tone-map enable (1 = HDR source, 0 = legacy LDR passthrough)
};

// Soft-knee highlight rolloff. This is the ONE place high-dynamic-range values re-enter an
// 8-bit buffer: every offscreen target is RGBA16F and the swapchain is not, so without a curve
// here everything above 1.0 would simply clip and the whole HDR path would be invisible.
//
// NOT Reinhard, which was the obvious first choice and is wrong for this project. Reinhard
// compresses the ENTIRE range, so nominal white maps to 0.53 and mid-grey to 0.34 -- it darkens
// every hull, star and UI sprite by roughly half. That is correct for a renderer whose art is
// authored in linear HDR, and incorrect here: this game's art is authored for an LDR target and
// only the additive VFX overshoot ever exceeds 1.0.
//
// So: IDENTITY below the knee, rational rolloff above it, asymptotic to 1.0. Below KNEE the
// output is bit-for-bit what the old 8-bit path produced, which means the change cannot regress
// the existing look -- it can only affect pixels that previously clipped. The rolloff's slope at
// the knee is exactly 1, so the two branches meet smoothly with no visible seam.
//
// Rational (over/(over+r)) rather than exponential, which was the first attempt: an exponential
// with the same C1 join saturates by ~2x nominal white, so a triple-barrel salvo would still
// flatten to one blob barely above a single shot. The rational tail keeps separating well past
// 8x -- 1x->0.90, 2x->0.97, 4x->0.988, 8x->0.995 -- which is the whole point of the exercise.
static const float TONEMAP_KNEE = 0.80;

float3 tonemap_soft(float3 c)
{
    const float k = TONEMAP_KNEE;
    const float r = 1.0 - k;                       // output headroom above the knee
    float3 over   = max(c - k, 0.0);
    float3 rolled = k + r * (over / (over + r));
    return min(c, rolled);                          // min() == identity below the knee
}

struct PSInput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target0
{
    float2 uv     = input.uv;
    float4 scene  = scene_tex.Sample(smp, uv);
    float4 bloom  = bloom_tex.Sample(smp, uv);
    float4 streak = aux_streak_tex.Sample(smp, uv);
    float4 flare  = aux_flare_tex.Sample(smp, uv);
    float3 hdr = scene.rgb + bloom.rgb * params.x + streak.rgb * params.y + flare.rgb * params.z;
    // Bloom/streak/flare are added BEFORE the curve, not after: they are light arriving at the
    // sensor, so they must be exposed along with everything else. Adding them afterwards would
    // let a glow push an already-tone-mapped pixel back past white and re-introduce the clipping
    // the curve exists to remove.
    float3 mapped = (params.w > 0.5) ? tonemap_soft(max(hdr, 0.0)) : hdr;
    return float4(mapped, scene.a);
}
