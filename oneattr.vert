#version 450

// Vertex-input config bisect, minimal variant: ONE attribute (loc0 f32vec2)
// with stride 36 (the replay/d2d stride).  If this fails while the
// sanity_const control (one attribute, stride 8) passes, the stride
// dimension alone breaks the vertex fetch on this PanVK stack.

layout(location = 0) in vec2 a0;

void main() {
    // a0 floats are 0.0 / 256.0 in the replay vertex data:
    //   v0 (0,0)->(-1,-1)  v1 (256,0)->(511,-1)  v2 (0,256)->(-1,511)
    gl_Position = vec4(a0 * 2.0 - 1.0, 0.5, 1.0);
}
