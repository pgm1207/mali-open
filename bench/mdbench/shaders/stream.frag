#version 450
layout(set=0,binding=1) uniform sampler2D tex0;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 o;
void main(){ o = vec4(texture(tex0, uv).rgb, 1.0); }
