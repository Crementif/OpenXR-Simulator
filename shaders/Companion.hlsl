// ImGui's atlas is white RGB with coverage in alpha. UI vertex colors are
// display-encoded; native preview textures are sampled as linear RGB.
[[vk::binding(0, 0), vk::combinedImageSampler]] Texture2D<float4> image;
[[vk::binding(0, 0), vk::combinedImageSampler]] SamplerState imageSampler;

struct Input
{
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
};

float3 Encode(float3 value)
{
    return select(value <= 0.0031308, value * 12.92, 1.055 * pow(max(value, 0), 1.0 / 2.4) - 0.055);
}

float3 Decode(float3 value)
{
    return select(value <= 0.04045, value / 12.92, pow((value + 0.055) / 1.055, 2.4));
}

float4 main(Input input) : SV_Target0
{
    float4 sampled = image.Sample(imageSampler, input.uv);
#ifdef SIMXR_SRGB_TARGET
    // The attachment performs the final display encoding.
    sampled.rgb *= Decode(input.color.rgb);
#else
    // A UNORM attachment has no automatic encoding. Leave UI colors intact.
    sampled.rgb = Encode(sampled.rgb) * input.color.rgb;
#endif
    return float4(sampled.rgb, sampled.a * input.color.a);
}
