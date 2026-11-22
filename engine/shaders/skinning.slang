// One palette row per joint, with three affine position rows followed by
// three inverse-transpose normal rows.
Texture2D<float4> s_skinPaletteTexture : register(t6);
SamplerState s_skinPaletteSampler : register(s6);

float3 skinVector(float4 vector, float4 joints, float4 weights, float firstColumn) {
    float3 result = 0;
    for (int i = 0; i < 4; ++i) {
        float weight = weights[i];
        if (weight > 0) {
            float row = (joints[i] + 0.5) / 256.0;
            float4 x = s_skinPaletteTexture.SampleLevel(s_skinPaletteSampler, float2((firstColumn + 0.5) / 6.0, row), 0);
            float4 y = s_skinPaletteTexture.SampleLevel(s_skinPaletteSampler, float2((firstColumn + 1.5) / 6.0, row), 0);
            float4 z = s_skinPaletteTexture.SampleLevel(s_skinPaletteSampler, float2((firstColumn + 2.5) / 6.0, row), 0);
            result += weight * float3(dot(x, vector), dot(y, vector), dot(z, vector));
        }
    }
    return result;
}

float3 skinPosition(float3 position, float4 joints, float4 weights) {
    return weights.x > 0 ? skinVector(float4(position, 1), joints, weights, 0) : position;
}

float3 skinNormal(float3 normal, float4 joints, float4 weights) {
    if (weights.x == 0) return normal;
    float3 blended = skinVector(float4(normal, 0), joints, weights, 3);
    float lengthSquared = dot(blended, blended);
    return lengthSquared > 1.17549435e-38 ? blended * rsqrt(lengthSquared) : normal;
}
