#version 450
// hwbench (c) geometry: positions arrive in NDC, one tiny triangle per three vertices, no shading work.
layout(location = 0) in vec2 p;
void main() { gl_Position = vec4(p, 0.5, 1.0); }
