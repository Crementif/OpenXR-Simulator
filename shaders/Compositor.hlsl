// Canonical geometry uses OpenXR right-handed -Z forward and top-left UVs.
// Inputs are native sRGB views when appropriate, so filtering is linear.
struct LayerConstants
{
    float4 eyeTangents;
    float4 sourceTangents;
    float4 sourceFromEyeRotation;
    float4 sourceFromEyePosition;
    float4 imageRectangle;
    float4 quadParameters;
    uint4 control;
};
#ifdef SIMXR_VULKAN
[[vk::push_constant]] ConstantBuffer<LayerConstants> layer;
[[vk::binding(0, 0)]] Texture2D<float4> sourceTexture : register(t0);
[[vk::binding(1, 0)]] SamplerState sourceSampler : register(s1);
#else
cbuffer LayerBuffer : register(b2)
{
    LayerConstants layer;
};
Texture2D<float4> sourceTexture : register(t0);
SamplerState sourceSampler : register(s1);
#endif

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};
VertexOutput VSMain(uint vertex : SV_VertexID)
{
    VertexOutput output;
    output.uv = float2((vertex << 1) & 2, vertex & 2);
    output.position = float4(output.uv.x * 2 - 1, 1 - output.uv.y * 2, 0, 1);
    return output;
}
float3 Rotate(float4 q, float3 v)
{
    return v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}
float4 PSMain(VertexOutput input) : SV_Target
{
    float3 eyeRay = float3(lerp(layer.eyeTangents.x, layer.eyeTangents.y, input.uv.x),
        lerp(layer.eyeTangents.z, layer.eyeTangents.w, input.uv.y), -1);
    float3 ray = Rotate(layer.sourceFromEyeRotation, eyeRay);
    float2 uv;
    if (layer.control.x == 0)
    {
        if (ray.z >= 0) discard;
        float2 tangent = ray.xy / -ray.z;
        uv = float2((tangent.x - layer.sourceTangents.x) / (layer.sourceTangents.y - layer.sourceTangents.x),
            (layer.sourceTangents.z - tangent.y) / (layer.sourceTangents.z - layer.sourceTangents.w));
    }
    else
    {
        if (layer.sourceFromEyePosition.z <= 0 || ray.z >= 0) discard;
        float distance = -layer.sourceFromEyePosition.z / ray.z;
        float2 hit = layer.sourceFromEyePosition.xy + distance * ray.xy;
        uv = float2(hit.x * layer.quadParameters.x + 0.5, 0.5 - hit.y * layer.quadParameters.y);
    }
    if (any(uv < 0) || any(uv > 1)) discard;
    uv = layer.imageRectangle.xy + uv * layer.imageRectangle.zw;
    uint width, height;
    sourceTexture.GetDimensions(width, height);
    float2 halfTexel = 0.5 / float2(width, height);
    uv = clamp(uv, layer.imageRectangle.xy + halfTexel,
        layer.imageRectangle.xy + layer.imageRectangle.zw - halfTexel);
    float4 color = sourceTexture.Sample(sourceSampler, uv);
    if ((layer.control.y & 1) == 0)
        color.a = 1;
    else if ((layer.control.y & 2) != 0)
        color.rgb *= color.a;
    return color; // Backend uses ONE / ONE_MINUS_SRC_ALPHA and sRGB output encode.
}
