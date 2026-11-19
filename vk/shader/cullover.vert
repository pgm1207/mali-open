#version 450
// t_compile MALI_CULLDIST: 4 clip + 8 cull distances (> 8: must fail)
layout(location=0) in vec4 pos;
layout(location=1) in vec4 col;
layout(location=0) out vec4 v_col;
layout(push_constant) uniform P { vec4 rot; } pc;
out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[4]; float gl_CullDistance[8]; };
void main(){
  vec2 p = vec2(pos.x * pc.rot.x - pos.y * pc.rot.y, pos.x * pc.rot.y + pos.y * pc.rot.x) + pc.rot.zw;
  gl_Position = vec4(p, 0.5, 1.0);
  for (int i = 0; i < 4; i++) gl_ClipDistance[i] = p.x; for (int i = 0; i < 8; i++) gl_CullDistance[i] = p.y;
  v_col = col;
}
