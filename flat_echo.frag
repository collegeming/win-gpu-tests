#version 450

// Flat/noperspective output delivery control for the D2D bisect.
// Pairs with the d2d_feed VS (same interface the real d2d FS consumes):
//   location 1  noperspective vec2  v_glyph
//   location 2  components 0..3     flat floats v_c0..v_c3
// The feed VS writes v_c0 = uintBitsToFloat(8u), v_c1 = 0.0f,
// v_c2 = uintBitsToFloat(1u), v_c3 = 1.0039f, v_glyph = (0,0).
// This shader echoes the received values as a byte-identifiable colour:
//   R = (bits(v_c0) & 15)/15      -> 8/15  -> byte 136
//   G = ((bits(v_c1) + bits(v_c2)) & 15)/15 -> 1/15 -> byte 17
//   B = v_glyph.x * 2.0            -> 0.0   -> byte 0
//   A = v_c3 * 0.5                 -> ~0.502 -> byte 128
// Expected BGRA8 bytes: {0, 17, 136, 128}.  If the flat outputs are
// delivered intact the sample points show exactly this; if the
// VS->FS interface delivery is broken the bytes deviate, which would
// reframe the real d2d FS's uniform (0,0,0,0) output as an
// input-delivery artifact rather than its own math.

layout(location = 1) noperspective in vec2 v_glyph;
layout(location = 2, component = 0) flat in float v_c0;
layout(location = 2, component = 1) flat in float v_c1;
layout(location = 2, component = 2) flat in float v_c2;
layout(location = 2, component = 3) flat in float v_c3;
layout(location = 0) out vec4 o_color;

void main() {
    uint b0 = floatBitsToUint(v_c0) & 15u; /* expect 8 */
    uint b1 = floatBitsToUint(v_c1) & 15u; /* expect 0 */
    uint b2 = floatBitsToUint(v_c2) & 15u; /* expect 1 */
    o_color = vec4(float(b0) / 15.0,
                   float(b1 + b2) / 15.0,
                   v_glyph.x * 2.0,
                   v_c3 * 0.5);
}
