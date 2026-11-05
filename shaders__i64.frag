#version 450
#extension GL_ARB_gpu_shader_int64 : require
layout(location=0) out vec4 o;
layout(push_constant) uniform P { uint a; uint b; } pc;
void main(){ uint64_t x=uint64_t(pc.a)*uint64_t(pc.b); o=vec4(float(uint(x>>32)),float(uint(x&0xffffu)),0,1); }
