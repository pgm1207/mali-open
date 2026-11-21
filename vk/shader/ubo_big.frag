#version 450
/* >1 MiB uniform buffer: the colour is placed beyond the 1 MiB mark so that a 1 MiB clamp (or a map
 * truncated at 1 MiB) cannot supply it. Layout is std140, so every vec4 below is 16-byte aligned:
 *   pad     0 .. 1 MiB - 16
 *   gap     1 MiB .. 1 MiB + 4096 - 16
 *   colour 1 MiB + 4096            <- the only word t_vk_draw --only ubo_big accepts */
layout(location = 0) in vec4 v_col;
layout(location = 0) out vec4 out_color;
layout(set = 0, binding = 0) uniform U {
    vec4 pad[65536];   /* 1 MiB */
    vec4 gap[256];     /* 4 KiB */
    vec4 colour;       /* offset 1 MiB + 4096 */
} u;
void main() { out_color = u.colour; }
