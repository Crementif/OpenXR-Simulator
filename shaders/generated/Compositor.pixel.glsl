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
    vec4 _306;
    do
    {
        if (layer.control.x == 2u)
        {
            float _89 = clamp((texelFetch(SPIRV_Cross_CombinedsourceTextureSPIRV_Cross_DummySampler, ivec3(ivec2(layer.imageRectangle.xy + (in_var_TEXCOORD0 * layer.imageRectangle.zw)), 0).xy, 0).x - layer.quadParameters.x) / (layer.quadParameters.y - layer.quadParameters.x), 0.0, 1.0);
            bool _96;
            if (layer.control.y != 0u)
            {
                _96 = _89 > 0.0;
            }
            else
            {
                _96 = false;
            }
            bool _100;
            if (_96)
            {
                _100 = _89 < 1.0;
            }
            else
            {
                _100 = false;
            }
            float _112;
            if (_100)
            {
                float _105 = layer.quadParameters.z * _89;
                _112 = _105 / ((layer.quadParameters.w * (1.0 - _89)) + _105);
            }
            else
            {
                _112 = _89;
            }
            _306 = vec4(_112, _112, _112, 1.0);
            break;
        }
        vec3 _128 = vec3(mix(layer.eyeTangents.x, layer.eyeTangents.y, in_var_TEXCOORD0.x), mix(layer.eyeTangents.z, layer.eyeTangents.w, in_var_TEXCOORD0.y), -1.0);
        vec3 _138 = _128 + (cross(layer.sourceFromEyeRotation.xyz, cross(layer.sourceFromEyeRotation.xyz, _128) + (_128 * layer.sourceFromEyeRotation.w)) * 2.0);
        float _139 = _138.z;
        bool _140 = _139 < 0.0;
        float _143 = (abs(_139) < 9.9999999747524270787835121154785e-07) ? (-9.9999999747524270787835121154785e-07) : _139;
        bool _247;
        vec2 _248;
        if (layer.control.x == 0u)
        {
            vec2 _151 = _138.xy / vec2(-_143);
            _247 = _140;
            _248 = vec2((_151.x - layer.sourceTangents.x) / (layer.sourceTangents.y - layer.sourceTangents.x), (layer.sourceTangents.z - _151.y) / (layer.sourceTangents.z - layer.sourceTangents.w));
        }
        else
        {
            bool _245;
            vec2 _246;
            if (layer.control.x == 3u)
            {
                vec3 _178 = layer.sourceFromEyePosition.xyz * layer.quadParameters.x;
                vec2 _179 = _138.xz;
                float _180 = dot(_179, _179);
                vec2 _181 = _178.xz;
                float _182 = dot(_181, _179);
                float _187 = (_182 * _182) - (_180 * (dot(_181, _181) - 1.0));
                float _192 = (sqrt(isnan(0.0) ? _187 : (isnan(_187) ? 0.0 : max(_187, 0.0))) - _182) / (isnan(9.9999996826552253889678874634872e-21) ? _180 : (isnan(_180) ? 9.9999996826552253889678874634872e-21 : max(_180, 9.9999996826552253889678874634872e-21)));
                vec3 _194 = _178 + (_138 * _192);
                bool _199;
                if (_180 > 0.0)
                {
                    _199 = _187 > 0.0;
                }
                else
                {
                    _199 = false;
                }
                bool _203;
                if (_199)
                {
                    _203 = _192 > 0.0;
                }
                else
                {
                    _203 = false;
                }
                _245 = _203;
                _246 = vec2(0.5 + (atan(_194.x, -_194.z) * layer.quadParameters.y), 0.5 - (_194.y * layer.quadParameters.z));
            }
            else
            {
                bool _223;
                if (_140)
                {
                    _223 = layer.sourceFromEyePosition.z > 0.0;
                }
                else
                {
                    _223 = false;
                }
                vec2 _233 = layer.sourceFromEyePosition.xy + (_138.xy * ((-layer.sourceFromEyePosition.z) / _143));
                _245 = _223;
                _246 = vec2((_233.x * layer.quadParameters.x) + 0.5, 0.5 - (_233.y * layer.quadParameters.y));
            }
            _247 = _245;
            _248 = _246;
        }
        vec2 _249 = dFdx(_248);
        vec2 _254 = dFdy(_248);
        bool _260;
        if (_247)
        {
            _260 = any(lessThan(_248, vec2(0.0)));
        }
        else
        {
            _260 = true;
        }
        bool _266;
        if (!_260)
        {
            _266 = any(greaterThan(_248, vec2(1.0)));
        }
        else
        {
            _266 = true;
        }
        if (_266)
        {
            discard;
        }
        uvec2 _273 = uvec2(textureSize(SPIRV_Cross_CombinedsourceTextureSPIRV_Cross_DummySampler, 0));
        vec2 _279 = vec2(0.5) / vec2(float(_273.x), float(_273.y));
        vec4 _287 = textureGrad(SPIRV_Cross_CombinedsourceTexturesourceSampler, clamp(layer.imageRectangle.xy + (_248 * layer.imageRectangle.zw), layer.imageRectangle.xy + _279, (layer.imageRectangle.xy + layer.imageRectangle.zw) - _279), _249 * layer.imageRectangle.zw, _254 * layer.imageRectangle.zw);
        vec4 _305;
        if ((layer.control.y & 1u) == 0u)
        {
            vec4 _295 = _287;
            _295.w = 1.0;
            _305 = _295;
        }
        else
        {
            vec4 _304;
            if ((layer.control.y & 2u) != 0u)
            {
                vec3 _302 = _287.xyz * _287.w;
                _304 = vec4(_302.x, _302.y, _302.z, _287.w);
            }
            else
            {
                _304 = _287;
            }
            _305 = _304;
        }
        _306 = _305;
        break;
    } while(false);
    out_var_SV_Target = _306;
}

