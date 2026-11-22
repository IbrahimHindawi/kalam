#include "fx_coverage.slang"
cbuffer MeshFx : register(b0) {
    float4x4 u_modelViewProj;
    float4 u_tint;
    float4 u_fxUvRect;
    float4 u_fxScroll;     // scroll XY, mask flip U/V
    float4 u_fxOpacity;    // alpha strength, opacity source, black threshold and softness
    float4 u_fxMaps;       // opacity present, ramp strength, noise present, animated phase
    float4 u_fxDissolve;   // hold enabled, hold fraction, softness
    float4 u_fxWave;       // enabled, frequency, speed, travel on V
    float4 u_fxWaveShape;  // width, softness, reverse
};
Texture2D s_colorTexture : register(t0);
SamplerState s_colorSampler : register(s0);
Texture2D s_fxOpacityTexture : register(t1);
SamplerState s_fxOpacitySampler : register(s1);
Texture2D s_fxRampTexture : register(t2);
SamplerState s_fxRampSampler : register(s2);
Texture2D s_fxDissolveTexture : register(t3);
SamplerState s_fxDissolveSampler : register(s3);

struct Surface {
    float4 clip : SV_Position;
    float2 uv : TEXCOORD0;
};
[shader("vertex")]
Surface vsMain(float3 position : POSITION, float2 uv : TEXCOORD0) {
    Surface s;
    s.clip = mul(u_modelViewProj, float4(position, 1));
    s.uv = uv;
    return s;
}

[shader("fragment")]
float4 fsMain(Surface v) : SV_Target {
    float2 uv = v.uv + u_fxScroll.xy;
    float2 mainUv = lerp(u_fxUvRect.xy, u_fxUvRect.zw, uv);
    float2 maskUv = lerp(uv, 1.0 - uv, u_fxScroll.zw);
    float4 texel = s_colorTexture.Sample(s_colorSampler, mainUv);
    float alpha = fxTextureAlpha(texel, u_fxOpacity);
    if (u_fxMaps.x > 0.5)
        alpha *= s_fxOpacityTexture.Sample(s_fxOpacitySampler, maskUv).r;
    if (u_fxMaps.y > 0) {
        float intensity = saturate(dot(texel.rgb, float3(.299, .587, .114)));
        float3 ramp = s_fxRampTexture.Sample(s_fxRampSampler, float2(intensity, .5)).rgb;
        texel.rgb = lerp(texel.rgb, ramp, u_fxMaps.y);
    }
    float phase = saturate(u_fxMaps.w);
    if (u_fxDissolve.x > 0.5) {
        float hold = clamp(u_fxDissolve.y, 0.0, .99);
        float progress = saturate((phase - hold) / (1.0 - hold));
        float edge = max(u_fxDissolve.z, .001);
        float noise = u_fxMaps.z > .5 ? s_fxDissolveTexture.Sample(s_fxDissolveSampler, maskUv).r : frac(maskUv.y);
        float cutoff = lerp(-edge, 1.0 + edge, progress);
        alpha *= progress <= 0 ? 1.0 : (progress >= 1.0 ? 0.0 : smoothstep(cutoff - edge, cutoff + edge, noise));
    } else if (u_fxWave.x > .5) {
        float width = max(u_fxWaveShape.x, .0001);
        float edge = max(u_fxWaveShape.y, .0001);
        float t = saturate(phase * max(abs(u_fxWave.z), .0001));
        if (u_fxWave.z < 0) t = 1.0 - t;
        if (u_fxWaveShape.z > .5) t = 1.0 - t;
        float2 axes = u_fxWave.w > .5 ? v.uv.yx : v.uv;
        float center = lerp(-width-edge, 1.0+width+edge, t) + sin(axes.y * u_fxWave.y) * width;
        alpha *= 1.0 - smoothstep(width, width + edge, abs(axes.x - center));
    }
    alpha = saturate(alpha * u_tint.a);
    return float4(texel.rgb * u_tint.rgb * alpha, alpha);
}
