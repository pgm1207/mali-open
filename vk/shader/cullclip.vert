#version 450
// t_compile MALI_CULLDIST: 2 clip + 2 cull distances
layout(location=0) in vec4 pos;
layout(location=1) in vec4 col;
layout(location=0) out vec4 v_col;
layout(push_constant) uniform P { vec4 rot; } pc;
out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[2]; float gl_CullDistance[2]; };
void main(){
  vec2 p = vec2(pos.x * pc.rot.x - pos.y * pc.rot.y, pos.x * pc.rot.y + pos.y * pc.rot.x) + pc.rot.zw;
  gl_Position = vec4(p, 0.5, 1.0);
  gl_ClipDistance[0] = p.x; gl_ClipDistance[1] = 1.0; gl_CullDistance[0] = p.y; gl_CullDistance[1] = 1.0;
  v_col = col;
}
