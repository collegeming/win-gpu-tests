#version 450
// Fullscreen triangle, split along the main diagonal: one half is drawn, the
// other half keeps the clear colour. At four samples the diagonal must resolve
// to a fringe of intermediate values rather than a hard step.
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p - 1.0, 0.0, 1.0);
}
