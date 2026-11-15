AMD FidelityFX Super Resolution 1 (FSR 1), the two headers `ffx_a.h` and `ffx_fsr1.h` from
https://github.com/GPUOpen-Effects/FidelityFX-FSR v1.0.2 (`ffx-fsr/`), unmodified. MIT license: LICENSE.txt.

Used by the Android present's upscale pass (`dm/android/vulkan_present.cpp`, namespace `upscale`, gear "Upscale" / `DM_GFX_UPSCALE`):
`dm/android/shaders/fsr_easu.frag` (EASU, edge-adaptive upscale) and `dm/android/shaders/fsr_rcas.frag` (RCAS sharpening after it).
Their SPIR-V is committed as `*.spv.inc` next to them, so the build does not compile GLSL; the command is in each shader's header.
