#version 450
// GPU self-test (xbg_selftest.cpp): a flat colour from the push constants.
// Regenerate xbg_selftest_spv.h after editing (the command is at the top of that file).
layout(push_constant) uniform Push { vec4 rect; vec4 color; float z; } pc;
layout(location = 0) out vec4 o;
void main() { o = pc.color; }
