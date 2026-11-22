// Evaluate black cutout on the original source before ramp and tint.
float fxTextureAlpha(float4 texel, float4 opacity) {
    float alpha = lerp(1.0, texel.a, saturate(opacity.x));
    if (opacity.y > 0.5) {
        float value = max(texel.r, max(texel.g, texel.b));
        float mask = opacity.w <= 0 ? float(value > opacity.z) :
            smoothstep(opacity.z, opacity.z + opacity.w, value);
        alpha = opacity.y < 1.5 ? mask : alpha * mask;
    }
    return alpha;
}
