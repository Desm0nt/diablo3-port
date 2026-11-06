#version 450
// GPU self-test (xbg_selftest.cpp): an axis-aligned rectangle (6 vertices, no vertex buffer) at one depth.
// Regenerate xbg_selftest_spv.h after editing (the command is at the top of that file).
layout(push_constant) uniform Push { vec4 rect; vec4 color; float z; } pc;
void main() {
    const vec2 c[6] = vec2[6](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(1, 0), vec2(1, 1), vec2(0, 1));
    gl_Position = vec4(mix(pc.rect.xy, pc.rect.zw, c[gl_VertexIndex]), pc.z, 1.0);
}
