#version 450
// R4 colour loadOp LOAD: re-emits the framebuffer's old contents (a linear image seen as a buffer) into the tile.
// push: row_bytes, kind (low byte, see below) | 0x100 = sRGB, bytes per texel.
layout(set = 0, binding = 0, std430) readonly buffer S { uint d[]; } src;
layout(push_constant) uniform P { uint row_bytes; uint flags; uint bpp; uint pad; } p;
// -DOUT_SAMPLE: multisampled loadOp LOAD. The pass has 4 samples and the fragment shader runs once per sample
// (sample shading, DCD0 evaluate-per-sample), so each invocation reads *its own* sample plane of the 4x image:
// pad carries the bytes per sample plane (the surface stride) and gl_SampleID selects it.
#ifdef OUT_SAMPLE
#define SAMPLE_OFF (uint(gl_SampleID) * p.pad)
#else
#define SAMPLE_OFF 0u
#endif
// Output type: the BLEND/tile conversion follows the NIR type of the colour output, so integer formats need an integer output (a float output is
// converted BY VALUE: bit-cast integers come out as 0). Variants (gen-meta-spv.sh): none = float, -DOUT_U = uvec4, -DOUT_S = ivec4.
#if defined(OUT_U) || defined(OUT_S)
#define OUT_INT
#endif
#ifdef OUT_U
layout(location = 0) out uvec4 o;
#elif defined(OUT_S)
layout(location = 0) out ivec4 o;
#else
layout(location = 0) out vec4 o;
#endif
float dec(float c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float f11(uint v, uint mbits) {   // unsigned small float: 5 exponent bits, mbits mantissa bits
   uint m = v & ((1u << mbits) - 1u), e = (v >> mbits) & 31u;
   float fm = float(m) / float(1u << mbits);
   if (e == 0u) return fm * exp2(-14.0);
   if (e == 31u) return m != 0u ? uintBitsToFloat(0x7fc00000u) : uintBitsToFloat(0x7f800000u);
   return (1.0 + fm) * exp2(float(e) - 15.0);
}
// RAW render-target kinds (integer / SNORM / UNORM16 / float32 formats): integers come out of the integer-typed variants (OUT_U / OUT_S) as raw
// values, SNORM / UNORM16 / float32 out of the float variant as floats.
// flags: kind 12 UINT, 13 SINT, 14 SNORM, 15 UNORM16, 16 FLOAT32, 17 A2B10G10R10 UINT, 18 A2B10G10R10 SINT; bits 12..14 = components.
uint ldc(uint byteoff, uint cb) {   // one component of cb (1, 2 or 4) bytes
   uint w = src.d[byteoff >> 2];
   if (cb == 4u) return w;
   uint v = w >> ((byteoff & 3u) * 8u);
   return v & (cb == 1u ? 0xffu : 0xffffu);
}
#ifdef OUT_INT
uvec4 raw_int(uint k, uint off, uint bpp, uint nc) {   // 12 UINT, 13 SINT, 17 A2B10G10R10 UINT, 18 SINT: raw component values
   uint cb = bpp / nc;
   uvec4 u = uvec4(0u, 0u, 0u, 1u);
   if (k >= 17u) {
      uint w = src.d[off >> 2];
      uvec4 v = uvec4(w & 1023u, (w >> 10) & 1023u, (w >> 20) & 1023u, w >> 30);
      if (k == 18u)
         return uvec4(ivec4(bitfieldExtract(int(v.x), 0, 10), bitfieldExtract(int(v.y), 0, 10), bitfieldExtract(int(v.z), 0, 10), bitfieldExtract(int(v.w), 0, 2)));
      return v;
   }
   for (uint i = 0u; i < 4u; i++) {
      if (i >= nc) break;
      uint v = ldc(off + i * cb, cb);
      u[i] = k == 12u ? v : uint(bitfieldExtract(int(v), 0, int(cb * 8u)));
   }
   return u;
}
void main() {
   uvec4 r;
   if ((p.flags & 255u) == 19u) {   // CmdClearAttachments quad: the clear words (the format's raw shader output) sit in the source buffer
      r = uvec4(src.d[0], src.d[1], src.d[2], src.d[3]);
   } else {
      uvec2 f = uvec2(gl_FragCoord.xy);
      r = raw_int(p.flags & 255u, f.y * p.row_bytes + f.x * p.bpp + SAMPLE_OFF, p.bpp, max((p.flags >> 12) & 7u, 1u));
   }
#ifdef OUT_U
   o = r;
#else
   o = ivec4(r);
#endif
}
#else
vec4 raw_flt(uint k, uint off, uint bpp, uint nc) {   // 14 SNORM, 15 UNORM16, 16 FLOAT32
   uint cb = bpp / nc;
   vec4 fv = vec4(0.0, 0.0, 0.0, 1.0);
   for (uint i = 0u; i < 4u; i++) {
      if (i >= nc) break;
      uint v = ldc(off + i * cb, cb);
      if (k == 14u) fv[i] = max(float(bitfieldExtract(int(v), 0, int(cb * 8u))) / float((1 << (cb * 8u - 1u)) - 1), -1.0);
      else if (k == 15u) fv[i] = float(v) / 65535.0;
      else fv[i] = uintBitsToFloat(v);
   }
   return fv;
}
void main() {
   if ((p.flags & 255u) == 19u) {   // CmdClearAttachments quad (float formats): the clear value floats sit in the source buffer
      o = uintBitsToFloat(uvec4(src.d[0], src.d[1], src.d[2], src.d[3]));
      return;
   }
   uvec2 f = uvec2(gl_FragCoord.xy);
   uint off = f.y * p.row_bytes + f.x * p.bpp + SAMPLE_OFF;
   uint w = src.d[off >> 2];
   uint sh = (off & 3u) * 8u;
   uint k = p.flags & 255u;
   if (k >= 14u) { o = raw_flt(k, off, p.bpp, max((p.flags >> 12) & 7u, 1u)); return; }
   vec4 c = vec4(0.0, 0.0, 0.0, 1.0);
   if (k <= 1u) {                                     // 0 = UNORM8 (R8/RG8/RGBA8), 1 = BGRA8
      uint v = w >> sh;
      if (p.bpp == 1u) v &= 0xffu; else if (p.bpp == 2u) v &= 0xffffu;
      c = unpackUnorm4x8(v);
      if (p.bpp < 4u) c.a = 1.0;
      if (p.bpp < 2u) c.g = 0.0;
      if (k == 1u) c = c.bgra;
   } else if (k == 2u) {                              // 16-bit float, 1/2/4 components
      uint v = (p.bpp == 2u) ? ((w >> sh) & 0xffffu) : w;
      vec2 a = unpackHalf2x16(v);
      c = vec4(a.x, p.bpp >= 4u ? a.y : 0.0, 0.0, 1.0);
      if (p.bpp == 8u) { vec2 b = unpackHalf2x16(src.d[(off >> 2) + 1u]); c.zw = b; }
      if (p.bpp == 2u) c.y = 0.0;
   } else if (k == 3u) {                              // B10G11R11_UFLOAT
      c = vec4(f11(w, 6u), f11(w >> 11, 6u), f11(w >> 22, 5u), 1.0);
   } else if (k <= 5u) {                              // 4 = A2B10G10R10, 5 = A2R10G10B10
      vec4 u = vec4(float(w & 1023u), float((w >> 10) & 1023u), float((w >> 20) & 1023u), float(w >> 30)) / vec4(1023.0, 1023.0, 1023.0, 3.0);
      c = k == 4u ? u : vec4(u.z, u.y, u.x, u.w);
   } else {
      uint v = (w >> sh) & 0xffffu;
      if (k <= 7u) {                                  // 6 = R5G6B5, 7 = B5G6R5
         c = vec4(float(v >> 11) / 31.0, float((v >> 5) & 63u) / 63.0, float(v & 31u) / 31.0, 1.0);
         if (k == 7u) c.rgb = c.bgr;
      } else if (k <= 9u) {                           // 8 = R4G4B4A4, 9 = B4G4R4A4
         c = vec4(float(v >> 12), float((v >> 8) & 15u), float((v >> 4) & 15u), float(v & 15u)) / 15.0;
         if (k == 9u) c.rgb = c.bgr;
      } else {                                        // 10 = R5G5B5A1, 11 = B5G5R5A1
         if ((p.flags & 0x40000u) != 0u) v = ((v & 0x7fffu) << 1) | (v >> 15);   // bit 18: A1R5G5B5 (alpha at bit 15) -> R5G5B5A1 layout
         c = vec4(float(v >> 11), float((v >> 6) & 31u), float((v >> 1) & 31u), float(v & 1u));
         c.rgb /= 31.0;
         if (k == 11u) c.rgb = c.bgr;
      }
   }
   if ((p.flags & 0x100u) != 0u) c.rgb = vec3(dec(c.r), dec(c.g), dec(c.b));
   o = c;
}
#endif
