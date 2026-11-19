#version 450

// Feed vertex shader for the internal bisect of the D2D text pair: replaces
// the real (suspect) D2D text VS so the real D2D FS can be probed alone.
// Fullscreen triangle from gl_VertexIndex (no vertex buffer, no UBO), with
// outputs matching the real FS input interface exactly:
//   location 1  noperspective vec2  glyph coords
//   location 2  components 0..3     flat floats c0..c3
//
// Output values reproduce what the replay triangle fed the FS:
//   v_glyph = (0,0)      -> FS clamps to 0
//   v_c0 = uintBitsToFloat(8u): FS does i32(Bitcast(c0)); bits 8 with glyph 0
//     gives b = (0+8)>>3 = 1, so all five texel fetches t0[b-1..b+3] hit
//     t0[0..4], in bounds with the all-zero dataset.
//   v_c1 = 0             -> second term of the b index stays 0.
//   v_c2 = uintBitsToFloat(1u): the FS row index becomes 1 under a bitcast
//     reading and 0 under a float->int convert reading; the harness LUT
//     stores rows 0 and 1 identical, so the fetch is deterministic either
//     way.
//   v_c3 = 1.0039        -> the brush value, as a plain float (>= 0 selects
//     the %111/%114 branch in the FS; min/fmix weights stay in range).
//
// With the all-zero t0 dataset the FS math then lands on LUT x=0/217/434,
// i.e. the designed SIG_ZERO output (20,200,10,200 BGRA) if it executes.

layout(location = 1) noperspective out vec2 v_glyph;
layout(location = 2, component = 0) flat out float v_c0;
layout(location = 2, component = 1) flat out float v_c1;
layout(location = 2, component = 2) flat out float v_c2;
layout(location = 2, component = 3) flat out float v_c3;

void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    v_glyph = vec2(0.0, 0.0);
    v_c0 = uintBitsToFloat(8u);
    v_c1 = 0.0;
    v_c2 = uintBitsToFloat(1u);
    v_c3 = 1.0039f;
}
