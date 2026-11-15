#version 450
#extension GL_GOOGLE_include_directive : require
// [D3] FSR 1 RCAS (AMD FidelityFX Super Resolution 1, MIT, Copyright (c) 2021 Advanced Micro Devices, Inc.; the unmodified headers in
// dm/third_party/fidelityfx-fsr1, docs/THIRD_PARTY.md): sharpens the EASU output (tex, the intermediate image of the same size) into the
// swapchain image, pixel for pixel. con0.x = exp2(-stops) of DM_GFX_SHARPEN (FsrRcasCon). vulkan_present.cpp namespace upscale.
// Offline compiled into fsr_rcas.frag.spv.inc:
//   glslc --target-env=vulkan1.1 -O -mfmt=num -I ../../third_party/fidelityfx-fsr1 fsr_rcas.frag -o fsr_rcas.frag.spv.inc
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(push_constant) uniform P { uvec4 con0; } pc;
layout(location = 0) out vec4 o;
#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"
#define FSR_RCAS_F 1
// clamped: the 3x3 cross must not read outside the image at its edges
AF4 FsrRcasLoadF(ASU2 p) { return texelFetch(tex, clamp(p, ASU2(0), textureSize(tex, 0) - 1), 0); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#include "ffx_fsr1.h"
void main() {
    AF3 c;
    FsrRcasF(c.r, c.g, c.b, AU2(gl_FragCoord.xy), pc.con0);
    o = vec4(c, 1.0);
}
