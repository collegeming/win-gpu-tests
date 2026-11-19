#version 450

// Reverse-confirm probe: fiveattr.vert (round-10 PASS, the proven healthy
// 5-attr VS with the real VS's bit-decode path) + `invariant gl_Position`.
// If the real VS dies ONLY because of the Invariant decoration on
// SV_Position, this module must reproduce the all-clear failure; if it
// still paints, Invariant alone is exonerated.
// Compares against round-11 vs_noinv (real VS minus Invariant).

layout(location = 0) in vec2 a0;
layout(location = 1) in vec2 a1;
layout(location = 2) in vec2 a2;
layout(location = 3) in vec2 a3;
layout(location = 4) in float a4;

invariant gl_Position;

void main() {
    vec2 p = vec2(floatBitsToInt(a0.x), floatBitsToInt(a0.y)) * 2.0 - 1.0;
    p += vec2(floatBitsToInt(a1.x), floatBitsToInt(a1.y));
    p += min(vec2(floatBitsToInt(a2.x), floatBitsToInt(a2.y)), 0.0);
    p += min(vec2(floatBitsToInt(a3.x), floatBitsToInt(a3.y)), 0.0);
    p += min(vec2(float(floatBitsToInt(a4))), 0.0);
    gl_Position = vec4(p, 0.5, 1.0);
}
