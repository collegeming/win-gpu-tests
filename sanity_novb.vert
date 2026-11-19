#version 450
// No-upload draw control for the D2D text shader replay harness
// (test_vk_d2d_replay.cpp, item sanity_novb). Same shape as the proven
// test_vk_uint_rt vertex shader: fullscreen triangle generated from
// gl_VertexIndex, NO vertex buffer, NO descriptors, NO CPU->GPU upload.
// Compiled to SPIR-V by the CI spirv job:
//   glslangValidator -V --target-env vulkan1.0 sanity_novb.vert -o sanity_novb_vert.spv
//   xxd -i -n sanity_novb_vs_spv sanity_novb_vert.spv > sanity_novb_vs_spv.h
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
