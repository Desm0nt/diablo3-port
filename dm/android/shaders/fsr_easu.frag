#version 450
#extension GL_GOOGLE_include_directive : require
// [D3] FSR 1 EASU (AMD FidelityFX Super Resolution 1, MIT, Copyright (c) 2021 Advanced Micro Devices, Inc.; the unmodified headers in
// dm/third_party/fidelityfx-fsr1, docs/THIRD_PARTY.md): the game's frame (its crop: the offset is in con0.zw, FsrEasuConOffset) scaled
// to the whole render target, one output pixel per fragment. vulkan_present.cpp namespace upscale (DM_GFX_UPSCALE=fsr, or auto on a
// GPU that is not Adreno). Offline compiled into fsr_easu.frag.spv.inc:
//   glslc --target-env=vulkan1.1 -O -mfmt=num -I ../../third_party/fidelityfx-fsr1 fsr_easu.frag -o fsr_easu.frag.spv.inc
layout(set = 0, binding = 0) uniform sampler2D tex;   // linear filter, clamp to edge
layout(push_constant) uniform P { uvec4 con0, con1, con2, con3; } pc;
layout(location = 0) out vec4 o;
#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return textureGather(tex, p, 0); }
AF4 FsrEasuGF(AF2 p) { return textureGather(tex, p, 1); }
AF4 FsrEasuBF(AF2 p) { return textureGather(tex, p, 2); }
#include "ffx_fsr1.h"
void main() {
    AF3 c;
    FsrEasuF(c, AU2(gl_FragCoord.xy), pc.con0, pc.con1, pc.con2, pc.con3);
    o = vec4(c, 1.0);
}
