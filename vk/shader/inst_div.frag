#version 450
// t_vk_feat inst_divisor: R = divisor-1 instance attribute, G = divisor-0 instance attribute, B = vertex-rate attribute.
layout(location = 0) in vec4 v_col;
layout(location = 0) out vec4 o;
void main(){ o = v_col; }
