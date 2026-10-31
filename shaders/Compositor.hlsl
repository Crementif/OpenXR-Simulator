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
#ifdef SIMXR_DEPTH_MS
[[vk::binding(0, 0)]] Texture2DMS<float4> sourceTexture : register(t0);
#else
[[vk::binding(0, 0)]] Texture2D<float4> sourceTexture : register(t0);
#endif
[[vk::binding(1, 0)]] SamplerState sourceSampler : register(s1);
#else
cbuffer LayerBuffer : register(b2)
{
    LayerConstants layer;
};
#ifdef SIMXR_DEPTH_MS
Texture2DMS<float4> sourceTexture : register(t0);
#else
Texture2D<float4> sourceTexture : register(t0);
#endif
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
    if (layer.control.x == 2)
    {
        int2 pixel = int2(layer.imageRectangle.xy + input.uv * layer.imageRectangle.zw);
        #ifdef SIMXR_DEPTH_MS
        float sample = sourceTexture.Load(pixel, 0).r;
#else
        float sample = sourceTexture.Load(int3(pixel, 0)).r;
#endif
        float value = saturate((sample - layer.quadParameters.x) / (layer.quadParameters.y - layer.quadParameters.x));
        if (layer.control.y != 0 && value > 0 && value < 1)
        {
            float numerator = layer.quadParameters.z * value;
            value = numerator / (layer.quadParameters.w * (1 - value) + numerator);
        }
        return float4(value, value, value, 1);
    }
#ifdef SIMXR_DEPTH_MS
    return float4(0, 0, 0, 1);
#else
    float3 eyeRay = float3(lerp(layer.eyeTangents.x, layer.eyeTangents.y, input.uv.x),
        lerp(layer.eyeTangents.z, layer.eyeTangents.w, input.uv.y), -1);
    float3 ray = Rotate(layer.sourceFromEyeRotation, eyeRay);
    float2 uv;
    bool visible = ray.z < 0;
    float rayZ = abs(ray.z) < 1e-6 ? -1e-6 : ray.z;
    if (layer.control.x == 0)
    {
        float2 tangent = ray.xy / -rayZ;
        uv = float2((tangent.x - layer.sourceTangents.x) / (layer.sourceTangents.y - layer.sourceTangents.x),
            (layer.sourceTangents.z - tangent.y) / (layer.sourceTangents.z - layer.sourceTangents.w));
    }
    else if (layer.control.x == 3)
    {
        float3 origin = layer.sourceFromEyePosition.xyz * layer.quadParameters.x;
        float a = dot(ray.xz, ray.xz);
        float b = dot(origin.xz, ray.xz);
        float c = dot(origin.xz, origin.xz) - 1;
        float discriminant = b * b - a * c;
        float distance = (-b + sqrt(max(discriminant, 0))) / max(a, 1e-20);
        float3 hit = origin + distance * ray;
        visible = a > 0 && discriminant > 0 && distance > 0;
        uv = float2(0.5 + atan2(hit.x, -hit.z) * layer.quadParameters.y,
            0.5 - hit.y * layer.quadParameters.z);
    }
    else
    {
        visible = visible && layer.sourceFromEyePosition.z > 0;
        float distance = -layer.sourceFromEyePosition.z / rayZ;
        float2 hit = layer.sourceFromEyePosition.xy + distance * ray.xy;
        uv = float2(hit.x * layer.quadParameters.x + 0.5, 0.5 - hit.y * layer.quadParameters.y);
    }
    float2 gradientX = ddx(uv) * layer.imageRectangle.zw;
    float2 gradientY = ddy(uv) * layer.imageRectangle.zw;
    if (!visible || any(uv < 0) || any(uv > 1)) discard;
    uv = layer.imageRectangle.xy + uv * layer.imageRectangle.zw;
    uint width, height;
    sourceTexture.GetDimensions(width, height);
    float2 halfTexel = 0.5 / float2(width, height);
    uv = clamp(uv, layer.imageRectangle.xy + halfTexel,
        layer.imageRectangle.xy + layer.imageRectangle.zw - halfTexel);
    float4 color = sourceTexture.SampleGrad(sourceSampler, uv, gradientX, gradientY);
    if ((layer.control.y & 1) == 0)
        color.a = 1;
    else if ((layer.control.y & 2) != 0)
        color.rgb *= color.a;
    return color; // Backend uses ONE / ONE_MINUS_SRC_ALPHA and sRGB output encode.
#endif
}
