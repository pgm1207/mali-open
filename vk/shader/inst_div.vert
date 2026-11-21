#version 450
// t_vk_feat inst_divisor: one triangle per instance (instance 0 the left half of the view, instance 1 the right half),
// with three vertex inputs read in the SAME draw so one readback proves all three:
//   location 0, binding 0: instance rate, divisor 1   -> element = instance index (two different colours)
//   location 1, binding 1: instance rate, divisor 0   -> one element, the same for every instance
//   location 2, binding 2: vertex rate                -> element = vertex index (varies across the triangle)
// The fragment stage writes those three values to R, G and B.
layout(location = 0) in vec4 instv;
layout(location = 1) in float zero_el;
layout(location = 2) in float vtx_el;
layout(location = 0) out vec4 v_col;
const vec2 p[3] = vec2[3](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0));
void main(){
  vec2 q = p[gl_VertexIndex].xy * vec2(0.5, 1.0) + vec2(gl_InstanceIndex == 0u ? -0.5 : 0.5, 0.0);
  gl_Position = vec4(q, 0.5, 1.0);
  v_col = vec4(instv.r, zero_el, vtx_el, 1.0);
}
