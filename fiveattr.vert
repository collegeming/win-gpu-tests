#version 450

// Loophole-closer control VS: the EXACT 5-attribute input interface of the
// real d2d VS (loc0-3 f32vec2 + loc4 f32, same 36-byte vertex buffer, same
// raw-u32-in-f32 data), decoded the way the real VS decodes it
// (bits -> i32 -> float).  Position from loc0 alone (the real VS derives
// SV_Position from loc0 + cb0 the same way: 2*ax - 1/64); a1..a4 consumed
// with provably-zero contribution for this dataset (min(x,0) of
// non-negative i32 decodes; NaN-free), so none of the five attributes can
// be eliminated by the compiler:
//   a1 bits 0 -> (0,0);  a2 bits(1) -> min(1,0)=0;
//   a3 bits(0x8001)/bits(0) -> min(...)=0;  a4 bits 0 -> 0.
// Resulting covering triangle: (-1,-1), (511,-1), (-1,511).
// PASS (all SIG_DESC) = the 5-attr/36B harness wiring is innocent and the
// d2d-VS execution verdict is final; FAIL = the wiring/config itself is
// the defect and the verdict reverses.

layout(location = 0) in vec2 a0;
layout(location = 1) in vec2 a1;
layout(location = 2) in vec2 a2;
layout(location = 3) in vec2 a3;
layout(location = 4) in float a4;

void main() {
    vec2 p = vec2(floatBitsToInt(a0.x), floatBitsToInt(a0.y)) * 2.0 - 1.0;
    p += vec2(floatBitsToInt(a1.x), floatBitsToInt(a1.y));
    p += min(vec2(floatBitsToInt(a2.x), floatBitsToInt(a2.y)), 0.0);
    p += min(vec2(floatBitsToInt(a3.x), floatBitsToInt(a3.y)), 0.0);
    p += min(vec2(float(floatBitsToInt(a4))), 0.0);
    gl_Position = vec4(p, 0.5, 1.0);
}
