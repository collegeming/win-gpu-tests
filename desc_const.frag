#version 450

// Descriptor-feeding control for the D2D replay harness.  Mirrors exactly the
// binding structure the real D2D pair sees: set1 = cb0 UBO (float4), set2 b0 =
// u32 texel buffer, set2 b1 = sampled image.  The vertex stage is the
// gl_VertexIndex fullscreen triangle (no vertex buffer), so rasterization does
// not depend on any uploaded data.  The fragment stage reads cb0.m.z (=-1 in
// the harness fill) and texel 0 of t0 (harness fills 63) and paints a colour
// that encodes both: R = -m.z (=1.0 -> 255 when the UBO is delivered),
// G = (t0[0] & 63)/63 (=1.0 -> 255 when the texel buffer is delivered),
// B = 0.75 (191), A = 1.0.  Expected BGRA bytes: {191, 255, 255, 255}.

layout(set = 1, binding = 0) uniform Cb0 { vec4 m; };
layout(set = 2, binding = 0) uniform usamplerBuffer t0;
layout(location = 0) out vec4 o_color;

void main() {
    uint v = texelFetch(t0, 0).r;
    o_color = vec4(-m.z, float(v & 63u) / 63.0, 0.75, 1.0);
}
