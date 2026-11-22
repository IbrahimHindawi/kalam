Texture2D s_colorTexture : register(t0);
SamplerState s_colorSampler : register(s0);

struct UiVertex {
    float4 clip : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

[shader("vertex")]
UiVertex vsMain(float2 position : POSITION, float4 color : COLOR0, float2 uv : TEXCOORD0) {
    UiVertex vertex;
    vertex.clip = float4(position, 0, 1);
    vertex.uv = uv;
    vertex.color = color;
    return vertex;
}

[shader("fragment")]
float4 fsMain(UiVertex vertex) : SV_Target {
    return vertex.color * s_colorTexture.Sample(s_colorSampler, vertex.uv);
}
