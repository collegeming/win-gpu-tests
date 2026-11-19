#version 450

// Vertex-input config bisect, minimal variant: ONE attribute (loc0 f32vec2)
// with stride 36 (the replay/d2d stride).  The vertex data stores RAW U32
// BIT PATTERNS in f32 slots, exactly as the real d2d VS is fed; this shader
// decodes them the same way the real VS does (bits -> i32 -> float), so
// loc0 = (0,0)/(256,0)/(0,256) as floats and gl_Position covers the frame:
//   v0 (-1,-1)  v1 (511,-1)  v2 (-1,511)
// If this fails while the sanity_const control (ordinary floats, stride 8)
// passes, the stride/attribute-shape dimension is the suspect on this stack.

layout(location = 0) in vec2 a0;

void main() {
    vec2 p = vec2(floatBitsToInt(a0.x), floatBitsToInt(a0.y)) * 2.0 - 1.0;
    gl_Position = vec4(p, 0.5, 1.0);
}
