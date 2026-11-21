#version 450
// hwbench (a) bandwidth: one NEAREST fetch per pixel, minimal ALU, 4-byte write.
layout(location = 0) in vec2 v_uv;
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) out vec4 o;
void main() {
   o = vec4(texture(tex, v_uv).rgb, 1.0);
}
