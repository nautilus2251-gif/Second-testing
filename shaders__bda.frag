#version 450
#extension GL_EXT_buffer_reference : require
layout(buffer_reference, std430) readonly buffer B { vec4 v; };
layout(push_constant) uniform P { B ptr; } pc;
layout(location=0) out vec4 o;
void main(){ o=pc.ptr.v; }
