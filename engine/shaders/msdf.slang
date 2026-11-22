cbuffer Msdf : register(b0) {
    // Signed distance range in atlas UV units, one component per axis.
    float4 u_msdfRange;
};
Texture2D s_colorTexture : register(t0);
SamplerState s_colorSampler : register(s0);

struct TextVertex {
    float4 clip : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

[shader("vertex")]
TextVertex vsMain(float2 position : POSITION, float4 color : COLOR0, float2 uv : TEXCOORD0) {
    TextVertex vertex;
    vertex.clip = float4(position, 0, 1);
    vertex.uv = uv;
    vertex.color = color;
    return vertex;
}

[shader("fragment")]
float4 fsMain(TextVertex vertex) : SV_Target {
    float3 sample = s_colorTexture.Sample(s_colorSampler, vertex.uv).rgb;
    float distance = max(min(sample.r, sample.g), min(max(sample.r, sample.g), sample.b));
    float2 pixelsPerUv = 1.0 / max(fwidth(vertex.uv), float2(0.000001));
    float pixelRange = max(0.5 * dot(u_msdfRange.xy, pixelsPerUv), 1.0);
    float coverage = saturate((distance - 0.5) * pixelRange + 0.5);
    return float4(vertex.color.rgb, vertex.color.a * coverage);
}
