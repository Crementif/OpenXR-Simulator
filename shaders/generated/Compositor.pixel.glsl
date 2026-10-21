#version 450

struct type_ConstantBuffer_LayerConstants
{
    vec4 eyeTangents;
    vec4 sourceTangents;
    vec4 sourceFromEyeRotation;
    vec4 sourceFromEyePosition;
    vec4 imageRectangle;
    vec4 quadParameters;
    uvec4 control;
};

uniform type_ConstantBuffer_LayerConstants layer;

layout(binding = 0) uniform sampler2D SPIRV_Cross_CombinedsourceTextureSPIRV_Cross_DummySampler;
layout(binding = 0) uniform sampler2D SPIRV_Cross_CombinedsourceTexturesourceSampler;

layout(location = 0) in vec2 in_var_TEXCOORD0;
layout(location = 0) out vec4 out_var_SV_Target;

void main()
{
    vec3 _65 = vec3(mix(layer.eyeTangents.x, layer.eyeTangents.y, in_var_TEXCOORD0.x), mix(layer.eyeTangents.z, layer.eyeTangents.w, in_var_TEXCOORD0.y), -1.0);
    vec3 _75 = _65 + (cross(layer.sourceFromEyeRotation.xyz, cross(layer.sourceFromEyeRotation.xyz, _65) + (_65 * layer.sourceFromEyeRotation.w)) * 2.0);
    vec2 _137;
    if (layer.control.x == 0u)
    {
        float _82 = _75.z;
        if (_82 >= 0.0)
        {
            discard;
        }
        vec2 _89 = _75.xy / vec2(-_82);
        _137 = vec2((_89.x - layer.sourceTangents.x) / (layer.sourceTangents.y - layer.sourceTangents.x), (layer.sourceTangents.z - _89.y) / (layer.sourceTangents.z - layer.sourceTangents.w));
    }
    else
    {
        bool _115;
        if (!(layer.sourceFromEyePosition.z <= 0.0))
        {
            _115 = _75.z >= 0.0;
        }
        else
        {
            _115 = true;
        }
        if (_115)
        {
            discard;
        }
        vec2 _125 = layer.sourceFromEyePosition.xy + (_75.xy * ((-layer.sourceFromEyePosition.z) / _75.z));
        _137 = vec2((_125.x * layer.quadParameters.x) + 0.5, 0.5 - (_125.y * layer.quadParameters.y));
    }
    bool _145;
    if (!any(lessThan(_137, vec2(0.0))))
    {
        _145 = any(greaterThan(_137, vec2(1.0)));
    }
    else
    {
        _145 = true;
    }
    if (_145)
    {
        discard;
    }
    uvec2 _155 = uvec2(textureSize(SPIRV_Cross_CombinedsourceTextureSPIRV_Cross_DummySampler, 0));
    vec2 _161 = vec2(0.5) / vec2(float(_155.x), float(_155.y));
    vec4 _169 = texture(SPIRV_Cross_CombinedsourceTexturesourceSampler, clamp(layer.imageRectangle.xy + (_137 * layer.imageRectangle.zw), layer.imageRectangle.xy + _161, (layer.imageRectangle.xy + layer.imageRectangle.zw) - _161));
    vec4 _187;
    if ((layer.control.y & 1u) == 0u)
    {
        vec4 _177 = _169;
        _177.w = 1.0;
        _187 = _177;
    }
    else
    {
        vec4 _186;
        if ((layer.control.y & 2u) != 0u)
        {
            vec3 _184 = _169.xyz * _169.w;
            _186 = vec4(_184.x, _184.y, _184.z, _169.w);
        }
        else
        {
            _186 = _169;
        }
        _187 = _186;
    }
    out_var_SV_Target = _187;
}

