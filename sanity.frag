#version 450
// Sanity control fragment shader for the D2D text shader replay harness
// (test_vk_d2d_replay.cpp, item sanity_const). Constant write; the target is
// BGRA8 so the readback bytes are (B,G,R,A) = (191,128,64,255) = SIG_SANITY.
// Compiled to SPIR-V by the CI spirv job:
//   glslangValidator -V --target-env vulkan1.0 sanity.frag -o sanity_frag.spv
//   xxd -i -n sanity_fs_spv sanity_frag.spv > sanity_fs_spv.h
layout(location = 0) out vec4 o_color;

void main() {
    o_color = vec4(64.0 / 255.0, 128.0 / 255.0, 191.0 / 255.0, 1.0);
}
