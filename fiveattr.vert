#version 450

// Healthy 5-attribute vertex-shader control for the D2D text bisect.
// Input interface identical to the real (suspect) D2D text VS:
//   location 0..3 = f32vec2, location 4 = f32, one 36-byte binding.
// This shader is ordinary compiler-generated GLSL (no hand-assembled
// SPIR-V), so if it also fails to rasterize through the same vertex
// buffer + input state, the defect is the HARNESS vertex-input wiring,
// not the d2d VS execution.
//
// The replay vertex buffer stores integer values 0/256 packed as raw
// f32 bit patterns in the a0 slots; read as ordinary floats they are
// 0.0 / 256.0:
//   v0 a0=(0,0)   -> pos (-1,-1)
//   v1 a0=(256,0) -> pos (511,-1)   (offscreen right, covers viewport)
//   v2 a0=(0,256) -> pos (-1,511)
// a1=(0,0) exactly; a2=intBitsToFloat(1)~1.4e-45 and a3.x~1.15e-41 are
// denormals that add ~0 (FTZ-safe: flushed or not, contribution < 1e-38);
// a4=0. All five attributes are consumed so none can be eliminated.

layout(location = 0) in vec2 a0;
layout(location = 1) in vec2 a1;
layout(location = 2) in vec2 a2;
layout(location = 3) in vec2 a3;
layout(location = 4) in float a4;

void main() {
    vec2 pos = a0 * 2.0 - 1.0;
    pos += a1;          // exactly (0,0) in the replay data
    pos += a2;          // denormal ~0
    pos += a3;          // denormal ~0
    pos += vec2(a4);    // 0
    gl_Position = vec4(pos, 0.5, 1.0);
}
