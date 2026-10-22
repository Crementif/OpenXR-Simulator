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

layout(binding = 0) uniform sampler2DMS SPIRV_Cross_CombinedsourceTextureSPIRV_Cross_DummySampler;

layout(location = 0) in vec2 in_var_TEXCOORD0;
layout(location = 0) out vec4 out_var_SV_Target;

void main()
{
    vec4 _90;
    do
    {
        if (layer.control.x == 2u)
        {
            float _65 = clamp((texelFetch(SPIRV_Cross_CombinedsourceTextureSPIRV_Cross_DummySampler, ivec2(layer.imageRectangle.xy + (in_var_TEXCOORD0 * layer.imageRectangle.zw)), 0).x - layer.quadParameters.x) / (layer.quadParameters.y - layer.quadParameters.x), 0.0, 1.0);
            bool _72;
            if (layer.control.y != 0u)
            {
                _72 = _65 > 0.0;
            }
            else
            {
                _72 = false;
            }
            bool _76;
            if (_72)
            {
                _76 = _65 < 1.0;
            }
            else
            {
                _76 = false;
            }
            float _88;
            if (_76)
            {
                float _81 = layer.quadParameters.z * _65;
                _88 = _81 / ((layer.quadParameters.w * (1.0 - _65)) + _81);
            }
            else
            {
                _88 = _65;
            }
            _90 = vec4(_88, _88, _88, 1.0);
            break;
        }
        _90 = vec4(0.0, 0.0, 0.0, 1.0);
        break;
    } while(false);
    out_var_SV_Target = _90;
}

