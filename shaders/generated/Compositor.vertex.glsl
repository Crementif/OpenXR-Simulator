#version 450

float _19;

layout(location = 0) out vec2 out_var_TEXCOORD0;

void main()
{
    float _24 = float((uint(gl_VertexID) << 1u) & 2u);
    float _26 = float(uint(gl_VertexID) & 2u);
    vec4 _31 = vec4((_24 * 2.0) - 1.0, _19, 0.0, 1.0);
    _31.y = (_26 * 2.0) - 1.0;
    gl_Position = _31;
    out_var_TEXCOORD0 = vec2(_24, _26);
    gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;
    gl_Position.y = -gl_Position.y;
}

