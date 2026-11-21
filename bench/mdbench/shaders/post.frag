#version 450
// megabonk post pass: 5-tap blur + vignette + tonemap over the offscreen scene colour
layout(set=0,binding=1) uniform sampler2D scene;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 o;
void main(){
  vec2 px = 1.0 / vec2(textureSize(scene, 0));
  vec3 c = texture(scene, uv).rgb * 0.4;
  c += texture(scene, uv + vec2(px.x, 0.0) * 1.5).rgb * 0.15;
  c += texture(scene, uv - vec2(px.x, 0.0) * 1.5).rgb * 0.15;
  c += texture(scene, uv + vec2(0.0, px.y) * 1.5).rgb * 0.15;
  c += texture(scene, uv - vec2(0.0, px.y) * 1.5).rgb * 0.15;
  vec2 d = uv - 0.5;
  c *= 1.0 - 0.5 * dot(d, d);
  c = c / (1.0 + c);
  o = vec4(pow(c, vec3(1.0 / 2.2)), 1.0);
}
