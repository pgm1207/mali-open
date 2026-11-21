#version 450
// hwbench (c) geometry: constant colour, no fetches, so nothing but the vertex/tiler path costs anything.
layout(location = 0) out vec4 o;
void main() { o = vec4(0.25, 0.5, 0.75, 1.0); }
