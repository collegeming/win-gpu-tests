#version 450
// Sanity control vertex shader for the D2D text shader replay harness
// (test_vk_d2d_replay.cpp, item sanity_const). One vec2 attribute at
// location 0, NDC passthrough. Compiled to SPIR-V by the CI spirv job:
//   glslangValidator -V --target-env vulkan1.0 sanity.vert -o sanity_vert.spv
//   xxd -i -n sanity_vs_spv sanity_vert.spv > sanity_vs_spv.h
// (replaces an earlier hand-assembled module whose OpMemoryModel and
// section layout were wrong for Mesa spirv_to_nir).
layout(location = 0) in vec2 a_pos;

void main() {
    gl_Position = vec4(a_pos, 0.5, 1.0);
}
