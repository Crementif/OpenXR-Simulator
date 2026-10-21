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
    vec4 _242;
    do
    {
        if (layer.control.x == 2u)
        {
            float _84 = clamp((texelFetch(SPIRV_Cross_CombinedsourceTextureSPIRV_Cross_DummySampler, ivec3(ivec2(layer.imageRectangle.xy + (in_var_TEXCOORD0 * layer.imageRectangle.zw)), 0).xy, 0).x - layer.quadParameters.x) / (layer.quadParameters.y - layer.quadParameters.x), 0.0, 1.0);
            bool _91;
            if (layer.control.y != 0u)
            {
                _91 = _84 > 0.0;
            }
            else
            {
                _91 = false;
            }
            bool _95;
            if (_91)
            {
                _95 = _84 < 1.0;
            }
            else
            {
                _95 = false;
            }
            float _107;
            if (_95)
            {
                float _100 = layer.quadParameters.z * _84;
                _107 = _100 / ((layer.quadParameters.w * (1.0 - _84)) + _100);
            }
            else
            {
                _107 = _84;
            }
            _242 = vec4(_107, _107, _107, 1.0);
            break;
        }
        vec3 _121 = vec3(mix(layer.eyeTangents.x, layer.eyeTangents.y, in_var_TEXCOORD0.x), mix(layer.eyeTangents.z, layer.eyeTangents.w, in_var_TEXCOORD0.y), -1.0);
        vec3 _131 = _121 + (cross(layer.sourceFromEyeRotation.xyz, cross(layer.sourceFromEyeRotation.xyz, _121) + (_121 * layer.sourceFromEyeRotation.w)) * 2.0);
        vec2 _191;
        if (layer.control.x == 0u)
        {
            float _136 = _131.z;
            if (_136 >= 0.0)
            {
                discard;
            }
            vec2 _143 = _131.xy / vec2(-_136);
            _191 = vec2((_143.x - layer.sourceTangents.x) / (layer.sourceTangents.y - layer.sourceTangents.x), (layer.sourceTangents.z - _143.y) / (layer.sourceTangents.z - layer.sourceTangents.w));
        }
        else
        {
            bool _169;
            if (!(layer.sourceFromEyePosition.z <= 0.0))
            {
                _169 = _131.z >= 0.0;
            }
            else
            {
                _169 = true;
            }
            if (_169)
            {
                discard;
            }
            vec2 _179 = layer.sourceFromEyePosition.xy + (_131.xy * ((-layer.sourceFromEyePosition.z) / _131.z));
            _191 = vec2((_179.x * layer.quadParameters.x) + 0.5, 0.5 - (_179.y * layer.quadParameters.y));
        }
        bool _199;
        if (!any(lessThan(_191, vec2(0.0))))
        {
            _199 = any(greaterThan(_191, vec2(1.0)));
        }
        else
        {
            _199 = true;
        }
        if (_199)
        {
            discard;
        }
        uvec2 _209 = uvec2(textureSize(SPIRV_Cross_CombinedsourceTextureSPIRV_Cross_DummySampler, 0));
        vec2 _215 = vec2(0.5) / vec2(float(_209.x), float(_209.y));
        vec4 _223 = texture(SPIRV_Cross_CombinedsourceTexturesourceSampler, clamp(layer.imageRectangle.xy + (_191 * layer.imageRectangle.zw), layer.imageRectangle.xy + _215, (layer.imageRectangle.xy + layer.imageRectangle.zw) - _215));
        vec4 _241;
        if ((layer.control.y & 1u) == 0u)
        {
            vec4 _231 = _223;
            _231.w = 1.0;
            _241 = _231;
        }
        else
        {
            vec4 _240;
            if ((layer.control.y & 2u) != 0u)
            {
                vec3 _238 = _223.xyz * _223.w;
                _240 = vec4(_238.x, _238.y, _238.z, _223.w);
            }
            else
            {
                _240 = _223;
            }
            _241 = _240;
        }
        _242 = _241;
        break;
    } while(false);
    out_var_SV_Target = _242;
}

