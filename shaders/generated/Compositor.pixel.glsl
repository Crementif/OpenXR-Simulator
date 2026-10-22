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
    vec4 _253;
    do
    {
        if (layer.control.x == 2u)
        {
            float _87 = clamp((texelFetch(SPIRV_Cross_CombinedsourceTextureSPIRV_Cross_DummySampler, ivec3(ivec2(layer.imageRectangle.xy + (in_var_TEXCOORD0 * layer.imageRectangle.zw)), 0).xy, 0).x - layer.quadParameters.x) / (layer.quadParameters.y - layer.quadParameters.x), 0.0, 1.0);
            bool _94;
            if (layer.control.y != 0u)
            {
                _94 = _87 > 0.0;
            }
            else
            {
                _94 = false;
            }
            bool _98;
            if (_94)
            {
                _98 = _87 < 1.0;
            }
            else
            {
                _98 = false;
            }
            float _110;
            if (_98)
            {
                float _103 = layer.quadParameters.z * _87;
                _110 = _103 / ((layer.quadParameters.w * (1.0 - _87)) + _103);
            }
            else
            {
                _110 = _87;
            }
            _253 = vec4(_110, _110, _110, 1.0);
            break;
        }
        vec3 _126 = vec3(mix(layer.eyeTangents.x, layer.eyeTangents.y, in_var_TEXCOORD0.x), mix(layer.eyeTangents.z, layer.eyeTangents.w, in_var_TEXCOORD0.y), -1.0);
        vec3 _136 = _126 + (cross(layer.sourceFromEyeRotation.xyz, cross(layer.sourceFromEyeRotation.xyz, _126) + (_126 * layer.sourceFromEyeRotation.w)) * 2.0);
        float _137 = _136.z;
        bool _138 = _137 < 0.0;
        float _141 = (abs(_137) < 9.9999999747524270787835121154785e-07) ? (-9.9999999747524270787835121154785e-07) : _137;
        bool _194;
        vec2 _195;
        if (layer.control.x == 0u)
        {
            vec2 _149 = _136.xy / vec2(-_141);
            _194 = _138;
            _195 = vec2((_149.x - layer.sourceTangents.x) / (layer.sourceTangents.y - layer.sourceTangents.x), (layer.sourceTangents.z - _149.y) / (layer.sourceTangents.z - layer.sourceTangents.w));
        }
        else
        {
            bool _172;
            if (_138)
            {
                _172 = layer.sourceFromEyePosition.z > 0.0;
            }
            else
            {
                _172 = false;
            }
            vec2 _182 = layer.sourceFromEyePosition.xy + (_136.xy * ((-layer.sourceFromEyePosition.z) / _141));
            _194 = _172;
            _195 = vec2((_182.x * layer.quadParameters.x) + 0.5, 0.5 - (_182.y * layer.quadParameters.y));
        }
        vec2 _196 = dFdx(_195);
        vec2 _201 = dFdy(_195);
        bool _207;
        if (_194)
        {
            _207 = any(lessThan(_195, vec2(0.0)));
        }
        else
        {
            _207 = true;
        }
        bool _213;
        if (!_207)
        {
            _213 = any(greaterThan(_195, vec2(1.0)));
        }
        else
        {
            _213 = true;
        }
        if (_213)
        {
            discard;
        }
        uvec2 _220 = uvec2(textureSize(SPIRV_Cross_CombinedsourceTextureSPIRV_Cross_DummySampler, 0));
        vec2 _226 = vec2(0.5) / vec2(float(_220.x), float(_220.y));
        vec4 _234 = textureGrad(SPIRV_Cross_CombinedsourceTexturesourceSampler, clamp(layer.imageRectangle.xy + (_195 * layer.imageRectangle.zw), layer.imageRectangle.xy + _226, (layer.imageRectangle.xy + layer.imageRectangle.zw) - _226), _196 * layer.imageRectangle.zw, _201 * layer.imageRectangle.zw);
        vec4 _252;
        if ((layer.control.y & 1u) == 0u)
        {
            vec4 _242 = _234;
            _242.w = 1.0;
            _252 = _242;
        }
        else
        {
            vec4 _251;
            if ((layer.control.y & 2u) != 0u)
            {
                vec3 _249 = _234.xyz * _234.w;
                _251 = vec4(_249.x, _249.y, _249.z, _234.w);
            }
            else
            {
                _251 = _234;
            }
            _252 = _251;
        }
        _253 = _252;
        break;
    } while(false);
    out_var_SV_Target = _253;
}

