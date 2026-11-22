cbuffer Sprite { float4 u_tint; };
Texture2D s_colorTexture : register(t0);
SamplerState s_colorSampler : register(s0);
struct Quad { float4 clip : SV_Position; float2 uv : TEXCOORD0; };
[shader("vertex")]
Quad vsMain(float3 position : POSITION, float2 uv : TEXCOORD0) {
    Quad q;
    q.clip = float4(position,1);
    q.uv = uv;
    return q;
}
[shader("fragment")]
float4 fsMain(Quad q) : SV_Target {
    return s_colorTexture.Sample(s_colorSampler, q.uv) * u_tint;
}
