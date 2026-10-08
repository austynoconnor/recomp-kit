// d3d9_ffp.h - the Direct3D fixed-function pipeline as generated shaders.
//
// A Direct3D 7/8/9 game that draws without shaders relies on the driver's
// fixed-function pipeline: transform, per-vertex lighting, texture
// coordinate generation and the texture stage cascade. The kit's renderers
// (the CPU rasterizer and the Metal, Vulkan and WebGPU backends) only run
// shader bytecode, so this module writes that bytecode: a vs_2_0 and a
// ps_2_0 program for the current state, cached by the state that shaped
// them, with their constant registers filled in for every draw. The
// renderers cannot tell the result from a game's own shaders.
//
// Alpha test and fog blending stay where they already are (the renderers
// apply them after the pixel shader); the vertex program writes oFog for
// vertex fog.
#pragma once
#include "d3d9_pipeline.h"

#include <cstdint>
#include <vector>

// D3DVERTEXELEMENT9 records (8 bytes each, ending with D3DDECL_END) for an
// FVF code, and the vertex size it describes. Empty for an FVF with no
// position.
std::vector<uint8_t> d9_fvf_declaration(uint32_t fvf);
uint32_t d9_fvf_stride(uint32_t fvf);

// Makes `out` a copy of `pl` whose empty vertex and/or pixel shader is
// replaced by the fixed-function equivalent, with its constants. `decl` is
// the bound declaration (D3DVERTEXELEMENT9 records), `cube_mask` the
// stages whose bound texture is a cube map and `bound_mask` the stages with
// any texture at all, `viewport` the viewport in pixels (X, Y, Width,
// Height) that pre-transformed vertices are mapped back through. False when
// nothing drawable can be made (no position).
bool d9_ffp_apply(const D9Pipeline &pl, const std::vector<uint8_t> &decl, uint32_t cube_mask,
                  uint32_t bound_mask, const int32_t viewport[4], D9Pipeline &out);
