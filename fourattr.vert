#version 450

// Vertex-input config bisect: FOUR attributes (loc0..3 f32vec2) with
// stride 32, i.e. the fiveattr shape WITHOUT the loc4 R32 fifth
// attribute.  Raw-u32-in-f32 data, decoded exactly like the real d2d VS
// (bits -> i32 -> float).  Position comes from loc0 alone (same as the
// real VS); a1..a3 are consumed with provably-zero contribution for this
// dataset (min(x,0) of non-negative i32 decodes; NaN-free): a1 bits 0 ->
// (0,0), a2 bits(1) -> min(1,0)=0, a3 bits(0x8001)/bits(0) -> min(...,0)=0.

layout(location = 0) in vec2 a0;
layout(location = 1) in vec2 a1;
layout(location = 2) in vec2 a2;
layout(location = 3) in vec2 a3;

void main() {
    vec2 p = vec2(floatBitsToInt(a0.x), floatBitsToInt(a0.y)) * 2.0 - 1.0;
    p += vec2(floatBitsToInt(a1.x), floatBitsToInt(a1.y));
    p += min(vec2(floatBitsToInt(a2.x), floatBitsToInt(a2.y)), 0.0);
    p += min(vec2(floatBitsToInt(a3.x), floatBitsToInt(a3.y)), 0.0);
    gl_Position = vec4(p, 0.5, 1.0);
}
