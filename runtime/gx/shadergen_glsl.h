#pragma once
#include "shadergen.h"

// Vulkan GLSL generation from the same keys as shadergen.cpp (the HLSL path for the PC
// backends), for the Android/Vulkan backend. The generated source is compiled to SPIR-V
// with shaderc, which ships prebuilt in the Android NDK, so the Android port uses a
// maintained, verified compiler instead of a bespoke one.
//
// The GLSL reproduces the *same* hardware semantics as the HLSL generator operation for
// operation: the 8-bit integer TEV registers, regular/comparison combiners with their
// exact bias/scale ordering, ksel swizzles, ksel konstant registers, indirect texturing
// (bias/matrix/S/T packing), the alpha range test, the 24-bit z-texture path, fog and the
// GX depth range. Vertex inputs use std430-free plain vertex attributes; constants are a
// single std140 buffer bound to both stages identical to VSConstants/PSConstants.
std::string GenerateVertexShaderGLSL(const VSKey& key);
std::string GenerateFragmentShaderGLSL(const PSKey& key);

// Clear / copy / EFB resolve helper shader (same helpers as the HLSL util shader).
// The vertex side is shared; the fragment side is either the solid fill or the copy.
std::string GenerateUtilShaderGLSL();
std::string GenerateUtilFragmentGLSL(bool copy);
