cbuffer Tone { float4 u_exposure; float4 u_outlineColor; };
Texture2D s_colorTexture : register(t0);
SamplerState s_colorSampler : register(s0);
struct Quad { float4 clip : SV_Position; float2 uv : TEXCOORD0; };
[shader("vertex")]
Quad vsMain(float3 position : POSITION, float2 uv : TEXCOORD0) {
    Quad q;
    q.clip = float4(position, 1);
#ifdef KALAM_GLES
    q.uv = float2(uv.x, 1.0 - uv.y);
#else
    q.uv = uv;
#endif
    return q;
}
float srgb(float x) {
    return x <= 0.0031308 ? 12.92*x : 1.055*pow(x,1.0/2.4)-0.055;
}
float calibrationPatch(int index) {
    switch (index) {
        case 0: return 0.0;
        case 1: return 0.001;
        case 2: return 0.01;
        case 3: return 0.05;
        case 4: return 0.18;
        case 5: return 0.5;
        case 6: return 1.0;
        default: return 4.0;
    }
}

bool selectionAt(float2 uv, float visibleDepth) {
    float depth = s_colorTexture.SampleLevel(s_colorSampler, uv, 0).a;
    return depth > 0.0 && depth <= visibleDepth;
}

bool outlineAt(float2 uv, float2 texel, float visibleDepth) {
    float2 x = float2(2.0 * texel.x, 0.0);
    float2 y = float2(0.0, 2.0 * texel.y);
    float2 diagonal = float2(texel.x, -texel.y);
    return selectionAt(uv + x, visibleDepth) || selectionAt(uv - x, visibleDepth)
        || selectionAt(uv + y, visibleDepth) || selectionAt(uv - y, visibleDepth)
        || selectionAt(uv + texel, visibleDepth) || selectionAt(uv - texel, visibleDepth)
        || selectionAt(uv + diagonal, visibleDepth) || selectionAt(uv - diagonal, visibleDepth);
}

[shader("fragment")]
float4 fsMain(Quad q) : SV_Target {
    float4 scene = s_colorTexture.Sample(s_colorSampler, q.uv);
    if (u_outlineColor.a > 0.0 && scene.a <= 0.0) {
        // The clear value is zero. An unselected surface can occlude the outline.
        float visibleDepth = scene.a == 0.0 ? 65504.0 : -scene.a;
        if (outlineAt(q.uv, 1.0 / u_exposure.zw, visibleDepth))
            return float4(u_outlineColor.rgb, 1.0);
    }
    float3 hdr = max(scene.rgb * u_exposure.x, 0.0);
    // Fixed scene-linear inputs exercise the same tone and sRGB path as the scene.
    float pixelY = q.clip.y;
#ifdef KALAM_GLES
    pixelY = u_exposure.w - pixelY;
#endif
    if (u_exposure.y > 0 && q.clip.x < u_exposure.z * 0.25 && pixelY >= u_exposure.w * 0.96) {
        int index = min(int(q.clip.x * 32.0 / u_exposure.z), 7);
        hdr = float3(calibrationPatch(index));
    }
    float3 mapped = hdr / (1.0 + hdr);
    return float4(srgb(mapped.r), srgb(mapped.g), srgb(mapped.b), 1.0);
}
