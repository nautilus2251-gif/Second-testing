#version 450
#extension GL_EXT_shader_explicit_arithmetic_types_int16 : require
layout(location=0) out vec4 o;
layout(push_constant) uniform P { uint a; } pc;
void main(){ int16_t x=int16_t(pc.a); o=vec4(float(x),0,0,1); }
