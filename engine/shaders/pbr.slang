// Scene-linear metallic/roughness model. Base color comes from an sRGB view.
#ifdef KALAM_SKINNED
#include "skinning.slang"
#endif
cbuffer Scene : register(b0)
{
    float4x4 u_modelViewProj;
    float4x4 u_model;
    float4x4 u_lightViewProj;
    float4 u_eye;
    float4 u_lightDir;
    float4 u_lightColor;
    float4 u_cameraFlags;
    float4 u_baseColor;
    float4 u_material;
};
Texture2D s_baseColorTexture : register(t0);
SamplerState s_baseColorSampler : register(s0);
Texture2D s_materialTexture : register(t1);
SamplerState s_materialSampler : register(s1);
Texture2D s_shadowTexture : register(t2);
SamplerState s_shadowSampler : register(s2);
TextureCube s_irradianceTexture : register(t3);
SamplerState s_irradianceSampler : register(s3);
TextureCube s_prefilterTexture : register(t4);
SamplerState s_prefilterSampler : register(s4);
Texture2D s_dfgTexture : register(t5);
SamplerState s_dfgSampler : register(s5);
Texture2D s_normalTexture : register(t7);
SamplerState s_normalSampler : register(s7);

struct Surface {
    float4 clip : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 lightClip : TEXCOORD3;
    float viewDepth : TEXCOORD4;
};

[shader("vertex")]
Surface vsMain(float3 position : POSITION, float3 normal : NORMAL, float2 uv : TEXCOORD0
#ifdef KALAM_SKINNED
               , float4 joints : TEXCOORD1, float4 weights : TEXCOORD2
#endif
)
{
#ifdef KALAM_SKINNED
    position = skinPosition(position, joints, weights);
    normal = skinNormal(normal, joints, weights);
#endif
    float4 p = float4(position, 1);
    Surface s;
    s.clip = mul(u_modelViewProj, p);
    s.viewDepth = s.clip.w;
    s.world = mul(u_model, p).xyz;
    // Cofactor transform handles nonuniform scale.
    float3x3 m = (float3x3)u_model;
    float3x3 cof = float3x3(cross(m[1], m[2]), cross(m[2], m[0]), cross(m[0], m[1]));
    s.normal = mul(cof, normal) * sign(determinant(m));
    s.uv = uv;
    s.lightClip = mul(u_lightViewProj, float4(s.world, 1));
    return s;
}

[shader("fragment")]
float4 fsMain(Surface s) : SV_Target
{
    float3 base = u_baseColor.rgb * s_baseColorTexture.Sample(s_baseColorSampler, s.uv).rgb;
    float4 mr = s_materialTexture.Sample(s_materialSampler, s.uv);
    float rough = clamp(u_material.y * mr.g, 0.045, 1.0);
    float metal = saturate(u_material.x * mr.b);
    float3 n = normalize(s.normal);
    // Derive the cotangent frame from the mesh UVs, including mirrored islands.
    float2 uvX = ddx(s.uv), uvY = ddy(s.uv);
    float3 posX = ddx(s.world), posY = ddy(s.world);
    float det = uvX.x * uvY.y - uvX.y * uvY.x;
    if (u_cameraFlags.w > 0.5 && abs(det) > 1e-9) {
        float3 tangent = (posX * uvY.y - posY * uvX.y) * sign(det);
        tangent -= n * dot(tangent, n);
        if (dot(tangent, tangent) > 1e-12) {
            tangent = normalize(tangent);
            float3 bitangent = cross(n, tangent);
            float3 reference = (posY * uvX.x - posX * uvY.x) * sign(det);
            bitangent *= dot(bitangent, reference) < 0.0 ? -1.0 : 1.0;
            float3 detail = s_normalTexture.SampleLevel(s_normalSampler, s.uv, 0).xyz * 2.0 - 1.0;
            n = normalize(tangent * detail.x + bitangent * detail.y + n * detail.z);
        }
    }
    float3 v = normalize(u_eye.xyz - s.world);
    float3 l = normalize(u_lightDir.xyz);
    float3 halfVec = v+l;
    float3 h = halfVec * rsqrt(max(dot(halfVec, halfVec), 1e-8));
    float nv = max(dot(n,v), 1e-4), nl = saturate(dot(n,l));
    float nh = saturate(dot(n,h)), vh = saturate(dot(v,h));
    float alpha = rough * rough, a2 = alpha * alpha;
    float denom = (1-nh*nh) + nh*nh*a2;
    float d = a2 / (3.14159265 * denom * denom);
    float vis = 0.5 / max(nl * sqrt(nv*nv*(1-a2)+a2) + nv * sqrt(nl*nl*(1-a2)+a2), 1e-5);
    float3 f0 = lerp(float3(0.04), base, metal);
    float3 f = f0 + (1-f0)*pow(1-vh, 5.0);
    float3 diffuse = base * (1-metal);
    float3 sc = s.lightClip.xyz / s.lightClip.w;
    float2 suv = float2(sc.x * 0.5 + 0.5, sc.y * u_cameraFlags.x + 0.5);
    float shadow = 1.0;
    if (all(suv >= 0) && all(suv <= 1) && sc.z > 0 && sc.z < 1)
        shadow = sc.z - u_cameraFlags.y <= s_shadowTexture.SampleLevel(s_shadowSampler, suv, 0).r ? 1.0 : 0.0;
    float3 direct = (diffuse / 3.14159265 + d*vis*f) * u_lightColor.rgb * nl * shadow;
    float3 irradiance = s_irradianceTexture.Sample(s_irradianceSampler,n).rgb;
    float3 reflection = s_prefilterTexture.SampleLevel(s_prefilterSampler,reflect(-v,n),rough*u_material.z).rgb;
    float2 ab = s_dfgTexture.Sample(s_dfgSampler,float2(nv,rough)).rg;
    // Alpha stores the nearest surface depth. Positive values mark selection.
    float outlineDepth = u_cameraFlags.z > 0.0 ? s.viewDepth : -s.viewDepth;
    return float4(direct + irradiance*diffuse + reflection*(f0*ab.x+ab.y) + base*u_material.w, outlineDepth);
}
