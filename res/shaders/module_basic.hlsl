cbuffer Cube_Constants : register(b0)
{
    row_major float4x4 model_view_projection;
    row_major float4x4 model;
    float4 cube_color;
    float4 post_tint;
    float4 post_parameters;
};

Texture2D surface_texture : register(t0);
SamplerState surface_sampler : register(s0);

struct Vertex_Input
{
    float3 position : POSITION;
    float3 normal : NORMAL;
};

struct Vertex_Output
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float2 screen_uv : TEXCOORD0;
};

Vertex_Output vs_main(Vertex_Input input)
{
    Vertex_Output output;
    output.position = mul(float4(input.position, 1.0f), model_view_projection);
    output.normal = mul(float4(input.normal, 0.0f), model).xyz;
    float2 ndc = output.position.xy / max(output.position.w, 0.0001f);
    output.screen_uv = float2(ndc.x * 0.5f + 0.5f, -ndc.y * 0.5f + 0.5f);
    return output;
}

float4 ps_main(Vertex_Output input) : SV_TARGET
{
    const float3 light_direction = normalize(float3(-0.35f, 0.65f, -0.68f));
    float diffuse = saturate(dot(normalize(input.normal), light_direction));
    float lighting = 0.30f + 0.70f * diffuse;
    float3 color = cube_color.rgb * lighting;
    float texel = surface_texture.Sample(
        surface_sampler,
        input.screen_uv * 4.0f).r;
    color *= 0.9f + 0.1f * texel;
    float brightest = max(color.r, max(color.g, color.b));
    float bloom = max(brightest - post_parameters.y, 0.0f);
    color += bloom * 0.20f;
    float2 centered = input.screen_uv * 2.0f - 1.0f;
    float edge = saturate(dot(centered, centered));
    color *= 1.0f - edge * post_parameters.z;
    color *= post_tint.rgb * post_parameters.x;
    return float4(color, cube_color.a * post_tint.a);
}
