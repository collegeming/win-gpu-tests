#version 450

// Vertex-input config bisect: TWO attributes (loc0, loc1 f32vec2) with
// stride 16.  Discriminates attribute-count >= 2 at a small stride: if
// this fails while sanity_const (1 attr, stride 8) passes, either the
// second attribute or stride 16 breaks the fetch; the 1-attr/stride-36
// and 4-attr/stride-32 variants separate the dimensions.

layout(location = 0) in vec2 a0;
layout(location = 1) in vec2 a1;

void main() {
    // a1 is exactly (0,0) in the bisect vertex data, so it contributes
    // nothing; it is consumed so the attribute cannot be eliminated.
    gl_Position = vec4(a0 * 2.0 - 1.0 + a1, 0.5, 1.0);
}
