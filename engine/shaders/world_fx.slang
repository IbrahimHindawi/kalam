#include "fx_coverage.slang"
cbuffer FxView : register(b0)
{
    float4x4 u_viewProj;
    float4 u_fxOpacity;
};
Texture2D s_colorTexture : register(t0);
SamplerState s_colorSampler : register(s0);

Texture2D s_fxRampTexture : register(t1);
SamplerState s_fxRampSampler : register(s1);
Texture2D s_fxDissolveTexture : register(t2);
SamplerState s_fxDissolveSampler : register(s2);

struct FxVertex {
    float4 clip : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float4 mask : TEXCOORD1;
};

[shader("vertex")]
FxVertex vsMain(float3 position : POSITION, float4 color : COLOR0, float2 uv : TEXCOORD0, float4 mask : TEXCOORD1) {
    FxVertex vertex;
    vertex.clip = mul(u_viewProj, float4(position, 1));
    vertex.uv = uv;
    vertex.color = color;
    vertex.mask = mask;
    return vertex;
}

[shader("fragment")]
float4 fsMain(FxVertex v) : SV_Target {
    float4 texel = s_colorTexture.Sample(s_colorSampler, v.uv);
    float coverage = fxTextureAlpha(texel, u_fxOpacity);
    if (v.mask.z > 0) {
        float intensity = saturate(dot(texel.rgb, float3(.299, .587, .114)));
        float3 ramp = s_fxRampTexture.Sample(s_fxRampSampler, float2(intensity, .5)).rgb;
        texel.rgb = lerp(texel.rgb, ramp, saturate(v.mask.z));
    }
    if (v.mask.w > 0) {
        float noise = s_fxDissolveTexture.Sample(s_fxDissolveSampler, v.mask.xy).r;
        if (noise < v.mask.w) coverage = 0;
    }
    coverage = saturate(coverage * v.color.a);
    return float4(texel.rgb * v.color.rgb * coverage, coverage);
}
