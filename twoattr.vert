#version 450

// Vertex-input config bisect: TWO attributes (loc0, loc1 f32vec2) with
// stride 16.  Raw-u32-in-f32 data, decoded exactly like the real d2d VS
// (bits -> i32 -> float): a0 gives the covering triangle, a1 is (0,0)
// (bits 0 -> i32 0), consumed so the attribute cannot be eliminated.

layout(location = 0) in vec2 a0;
layout(location = 1) in vec2 a1;

void main() {
    vec2 p0 = vec2(floatBitsToInt(a0.x), floatBitsToInt(a0.y));
    vec2 p1 = vec2(floatBitsToInt(a1.x), floatBitsToInt(a1.y));
    gl_Position = vec4(p0 * 2.0 - 1.0 + p1, 0.5, 1.0);
}
