#version 450

// Vertex-input config bisect: FOUR attributes (loc0..3 f32vec2) with
// stride 32, i.e. the fiveattr shape WITHOUT the loc4 R32 fifth
// attribute.  If fiveattr (5 attrs, stride 36) fails but this passes,
// the killer is the fifth R32 attribute / its offset / the 36-byte
// stride, not the four R32G32 attributes.

layout(location = 0) in vec2 a0;
layout(location = 1) in vec2 a1;
layout(location = 2) in vec2 a2;
layout(location = 3) in vec2 a3;

void main() {
    // a1=(0,0) exactly; a2,a3 are denormals (~1e-44) that add nothing,
    // FTZ-safe; all four consumed so none can be eliminated.
    gl_Position = vec4(a0 * 2.0 - 1.0 + a1 + a2 + a3, 0.5, 1.0);
}
