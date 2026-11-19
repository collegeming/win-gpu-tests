#version 450
#extension GL_EXT_samplerless_texture_functions : require

// t1 readback control for the D2D replay harness.  The real d2d FS wrote
// uniform (0,0,0,0) through the feed VS; three explanations are live:
//   H1: OpImageFetch on the 2D sampled image returns zeros on this PanVK
//   H2: the harness staging->image pre-pass upload never lands
//   H3: the real FS's own index math / flat inputs produce OOB coords
// This control fetches KNOWN LUT texels through the exact same upload
// path (same staging buffer, same pre-pass copy + barriers, same
// descriptor set, same SAMPLED_IMAGE descriptor type; texelFetch on a
// samplerless texture2D lowers to OpImageFetch, the same opcode the
// real FS uses).  A PASS here kills H2 and the generic part of H1 and
// leaves the anomaly inside the real FS's own computation (H3); a FAIL
// realizes H2 (harness upload defect) and exonerates the real FS.
//
// LUT design values (row 0 and row 1 identical, .z channel):
//   (0, 0).z   = 10/255
//   (434, 0).z = 20/255
//   (0, 1).z   = 10/255   (row-1 == row-0 check)
// Encoded output: R = (0,0).z, G = const 0.5, B = (434,0).z,
// A = (0,1).z.  Expected FS out (10/255, 0.5, 20/255, 10/255) ->
// BGRA8 bytes {20, 128, 10, 10}: exact UNORM round-trips
// (20/255<->20, 10/255<->10, 0.5<->128).  Distinct from every other
// signature in the harness.  If the upload/fetch is broken the R and B
// channels drop to 0 while G stays 128 (clear colour is unreachable:
// its R byte is 10, here R = fetched .z at x=0 -> 10 only if healthy
// ... a broken fetch gives {0,128,0,10}, still non-clear).

layout(set = 2, binding = 1) uniform texture2D t1;
layout(location = 0) out vec4 o_color;

void main() {
    float r  = texelFetch(t1, ivec2(0, 0), 0).z;    // expect 10/255
    float b  = texelFetch(t1, ivec2(434, 0), 0).z; // expect 20/255
    float r1 = texelFetch(t1, ivec2(0, 1), 0).z;   // expect 10/255
    o_color = vec4(r, 0.5, b, r1);
}
