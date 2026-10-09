// d3dx9_38.cpp - D3DX 9 as a March 2008 SDK game links it (d3dx9_38.dll).
//
// Every d3dx9_26 export the kit implements keeps its behaviour under the newer
// DLL name; the interfaces did not change between the two. On top of that, a
// Gamebryo-era game (Bully: Scholarship Edition) calls the rest of this list:
//
//  - D3DXCreateEffect over an fx_2_0 blob it loaded itself (.fxb files):
//    parsed by the same effect code as resource-loaded effects.
//  - The shader utilities: version and input-semantic queries read the
//    bytecode itself; the profile queries name what the device's caps report
//    (vs_3_0 / ps_3_0).
//  - D3DXCreateBuffer: an ID3DXBuffer, which has ID3DBlob's exact vtable.
//  - D3DXCreateTexture: the device's own CreateTexture, called through its
//    vtable so the texture is an ordinary device texture.
//  - D3DXMatrixReflect: real maths.
//
// Run-time HLSL compiling serves a table of known sources (see below); any
// other source fails. What fails cleanly, each reported once by name:
// assembling, the effect compiler, the shader constant table, and the
// image-file loaders and savers. A game that only uses its precompiled
// effects never reaches them; one that does shows exactly which it needs.
#include "com.h"
#include "d3d11.h"
#include "dx.h"
#include "../runtime/guest.h"
#include "../runtime/memory.h"

#include <math.h>
#include <string.h>
#include <iterator>
#include <string>
#include <vector>

void d3dx9_create_effect(X86 *c, uint32_t device, const uint8_t *blob, uint32_t size,
                         const char *name, uint32_t pool_view, uint32_t out);

namespace {

const uint32_t D3D_OK_ = 0;
const uint32_t D3DERR_INVALIDCALL_ = 0x8876086cu;
const uint32_t D3DERR_NOTAVAILABLE_ = 0x8876086au;
const uint32_t D3DXERR_INVALIDDATA_ = 0x88760b59u;
const uint32_t D3D_DEVICE9_CREATETEXTURE = 23;

void zero_out(uint32_t p) {
    if (p && gm_valid(p, 4))
        wr32(p, 0);
}

// One report per entry point: enough to say what a game needs, not a flood.
void report_once(const char *what) {
    static std::vector<std::string> seen;
    for (const std::string &s : seen)
        if (s == what)
            return;
    seen.emplace_back(what);
    LOGW("d3dx9_38: %s is not supported; the call fails cleanly", what);
}

// (pDevice, pSrcData, SrcDataLen, pDefines, pInclude, Flags, pPool,
//  ppEffect, ppCompilationErrors)
void X_D3DXCreateEffect(X86 *c) {
    uint32_t device = arg(c, 0), src = arg(c, 1), len = arg(c, 2), out = arg(c, 7);
    zero_out(arg(c, 8));
    if (!out || !src || !len || !gm_valid(src, len)) {
        set_eax(c, D3DERR_INVALIDCALL_);
        return;
    }
    char name[32];
    snprintf(name, sizeof name, "memory@%08x", src);
    d3dx9_create_effect(c, device, gm_ptr(src), len, name, arg(c, 6), out);
}

// Fail-clean shims, each with its output pointers cleared first.
template <int Out1, int Out2, int Out3> void fails(X86 *c, const char *what, uint32_t hr) {
    if (Out1 >= 0)
        zero_out(arg(c, Out1));
    if (Out2 >= 0)
        zero_out(arg(c, Out2));
    if (Out3 >= 0)
        zero_out(arg(c, Out3));
    report_once(what);
    set_eax(c, hr);
}
// Run-time HLSL a game compiles from source strings in its own executable is
// served from this table instead: each entry is the exact source (FNV-1a 64
// over its bytes, without a trailing NUL) and the shader model 2 bytecode a
// compiler produces for it, assembled by hand. An unknown source fails
// closed, as a missing compiler would.
//
// Bully: Scholarship Edition's 2D helper (0x00738fe0) compiles five:
//   1  vs_2_0  mViewProjection; POSITION, COLOR (UBYTE4 as bgra / 255)
//   2  vs_2_0  mViewProjection; POSITION, TEXCOORD0, COLOR
//   3  vs_2_0  mProjection; screen-space POSITION (x,y in 0..1, y down),
//              z/w from the projection, TEXCOORD0, COLOR
//   4  ps_2_0  COLOR0
//   5  ps_2_0  tex2D(s0, TEXCOORD0) * COLOR0
struct KnownShader {
    uint64_t hash;
    const uint32_t *tokens;
    uint32_t count;
};
// def c4, 1/255, 2, -1, 1
#define DEF_C4 0x05000051u, 0xa00f0004u, 0x3b808081u, 0x40000000u, 0xbf800000u, 0x3f800000u
#define DCL(usage, reg) 0x0200001fu, (usage), (reg)
#define DP4_POS(mask, creg)                                                                        \
    0x03000009u, 0xc0000000u | ((mask) << 16), 0x90e40000u, 0xa0e40000u | (creg)
const uint32_t k_vs_color[] = {0xfffe0200u,
                               DEF_C4,
                               DCL(0x80000000u, 0x900f0000u),
                               DCL(0x8000000au, 0x900f0001u),
                               DP4_POS(1, 0),
                               DP4_POS(2, 1),
                               DP4_POS(4, 2),
                               DP4_POS(8, 3),
                               0x03000005u,
                               0xd00f0000u,
                               0x90c60001u,
                               0xa0000004u, // mul oD0, v1.zyxw, c4.x
                               0x0000ffffu};
const uint32_t k_vs_textured[] = {0xfffe0200u,
                                  DEF_C4,
                                  DCL(0x80000000u, 0x900f0000u),
                                  DCL(0x80000005u, 0x900f0001u),
                                  DCL(0x8000000au, 0x900f0002u),
                                  DP4_POS(1, 0),
                                  DP4_POS(2, 1),
                                  DP4_POS(4, 2),
                                  DP4_POS(8, 3),
                                  0x02000001u,
                                  0xe0030000u,
                                  0x90e40001u, // mov oT0.xy, v1
                                  0x03000005u,
                                  0xd00f0000u,
                                  0x90c60002u,
                                  0xa0000004u, // mul oD0, v2.zyxw, c4.x
                                  0x0000ffffu};
const uint32_t k_vs_screen[] = {0xfffe0200u,
                                DEF_C4,
                                0x05000051u,
                                0xa00f0005u,
                                0xc0000000u,
                                0x3f800000u,
                                0u,
                                0u, // def c5, -2, 1, 0, 0
                                DCL(0x80000000u, 0x900f0000u),
                                DCL(0x80000005u, 0x900f0001u),
                                DCL(0x8000000au, 0x900f0002u),
                                0x04000004u,
                                0xc0010000u,
                                0x90000000u,
                                0xa0550004u,
                                0xa0aa0004u, // mad oPos.x, v0.x, c4.y, c4.z
                                0x04000004u,
                                0xc0020000u,
                                0x90550000u,
                                0xa0000005u,
                                0xa0550005u, // mad oPos.y, v0.y, c5.x, c5.y
                                0x03000009u,
                                0x80040000u,
                                0x90e40000u,
                                0xa0e40002u, // dp4 r0.z, v0, c2
                                0x03000009u,
                                0x80080000u,
                                0x90e40000u,
                                0xa0e40003u, // dp4 r0.w, v0, c3
                                0x02000006u,
                                0x80080001u,
                                0x80ff0000u, // rcp r1.w, r0.w
                                0x03000005u,
                                0xc0040000u,
                                0x80aa0000u,
                                0x80ff0001u, // mul oPos.z, r0.z, r1.w
                                0x02000001u,
                                0xc0080000u,
                                0xa0ff0004u, // mov oPos.w, c4.w
                                0x02000001u,
                                0xe0030000u,
                                0x90e40001u, // mov oT0.xy, v1
                                0x03000005u,
                                0xd00f0000u,
                                0x90c60002u,
                                0xa0000004u, // mul oD0, v2.zyxw, c4.x
                                0x0000ffffu};
const uint32_t k_ps_color[] = {0xffff0200u, DCL(0x80000000u, 0x900f0000u), // dcl v0
                               0x02000001u, 0x800f0800u,
                               0x90e40000u, // mov oC0, v0
                               0x0000ffffu};
const uint32_t k_ps_textured[] = {0xffff0200u,
                                  DCL(0x80000000u, 0xb0030000u), // dcl t0.xy
                                  DCL(0x80000000u, 0x900f0000u), // dcl v0
                                  DCL(0x90000000u, 0xa00f0800u), // dcl_2d s0
                                  0x03000042u,
                                  0x800f0000u,
                                  0xb0e40000u,
                                  0xa0e40800u, // texld r0, t0, s0
                                  0x03000005u,
                                  0x800f0000u,
                                  0x80e40000u,
                                  0x90e40000u, // mul r0, r0, v0
                                  0x02000001u,
                                  0x800f0800u,
                                  0x80e40000u, // mov oC0, r0
                                  0x0000ffffu};
#undef DEF_C4
#undef DCL
#undef DP4_POS
const KnownShader k_known[] = {
    {0xc7b0b46adb6a0406ull, k_vs_color, (uint32_t)std::size(k_vs_color)},
    {0x12127856d6bde5b3ull, k_vs_textured, (uint32_t)std::size(k_vs_textured)},
    {0xae9a0e8ebd4d1a26ull, k_vs_screen, (uint32_t)std::size(k_vs_screen)},
    {0x98af7d134bce13e2ull, k_ps_color, (uint32_t)std::size(k_ps_color)},
    {0xa75b3201187a9474ull, k_ps_textured, (uint32_t)std::size(k_ps_textured)},
};

uint32_t make_buffer(const void *bytes, uint32_t size) {
    ComObj *obj = dx11::create(K_D3D_BLOB, IF_D3D_BLOB);
    dx11::Object *o = obj ? dx11::get(obj) : nullptr;
    if (!o)
        return 0;
    o->bytes = size;
    o->data = size ? heap_alloc(size, true, 16) : 0;
    if (size && !o->data) {
        com_release(obj);
        return 0;
    }
    if (size)
        memcpy(gm_ptr(o->data), bytes, size);
    return com_view(obj, IF_D3D_BLOB);
}

// D3DXCompileShader(src, len, defines, include, function, profile, flags,
//                   ppShader, ppErrorMsgs, ppConstantTable)
void X_D3DXCompileShader(X86 *c) {
    uint32_t src = arg(c, 0), len = arg(c, 1), out = arg(c, 7);
    zero_out(out);
    zero_out(arg(c, 8));
    zero_out(arg(c, 9));
    if (src && len && gm_valid(src, len)) {
        if (rd8(src + len - 1) == 0)
            --len;
        uint64_t h = 14695981039346656037ull;
        for (uint32_t i = 0; i < len; ++i) {
            h ^= rd8(src + i);
            h *= 1099511628211ull;
        }
        for (const KnownShader &k : k_known)
            if (k.hash == h) {
                uint32_t view = out ? make_buffer(k.tokens, 4u * k.count) : 0;
                if (!view) {
                    set_eax(c, out ? E_OUTOFMEMORY : D3DERR_INVALIDCALL_);
                    return;
                }
                wr32(out, view);
                LOGV("d3dx9_38: D3DXCompileShader served known source %016llx (%u tokens)",
                     (unsigned long long)h, k.count);
                set_eax(c, D3D_OK_);
                return;
            }
        LOGW("d3dx9_38: D3DXCompileShader: unknown source %016llx (%u bytes) fails",
             (unsigned long long)h, len);
    }
    set_eax(c, E_FAIL);
}
// D3DXCompileShaderFromFileA(file, defines, include, function, profile, flags,
//                            ppShader, ppErrorMsgs, ppConstantTable)
void X_D3DXCompileShaderFromFileA(X86 *c) {
    fails<6, 7, 8>(c, "D3DXCompileShaderFromFileA", E_FAIL);
}
// D3DXAssembleShader(src, len, defines, include, flags, ppShader, ppErrorMsgs)
void X_D3DXAssembleShader(X86 *c) {
    fails<5, 6, -1>(c, "D3DXAssembleShader", E_FAIL);
}
// D3DXAssembleShaderFromFileA(file, defines, include, flags, ppShader, ppErrorMsgs)
void X_D3DXAssembleShaderFromFileA(X86 *c) {
    fails<4, 5, -1>(c, "D3DXAssembleShaderFromFileA", E_FAIL);
}
// D3DXCreateEffectCompiler(src, len, defines, include, flags, ppCompiler, ppErrors)
void X_D3DXCreateEffectCompiler(X86 *c) {
    fails<5, 6, -1>(c, "D3DXCreateEffectCompiler", E_FAIL);
}
// D3DXGetShaderConstantTable(pFunction, ppConstantTable)
void X_D3DXGetShaderConstantTable(X86 *c) {
    fails<1, -1, -1>(c, "D3DXGetShaderConstantTable", D3DXERR_INVALIDDATA_);
}
// D3DXCreateTextureFromFileInMemory(device, src, len, ppTexture)
void X_D3DXCreateTextureFromFileInMemory(X86 *c) {
    fails<3, -1, -1>(c, "D3DXCreateTextureFromFileInMemory", D3DXERR_INVALIDDATA_);
}
// D3DXCreateTextureFromFileExA(device, file, w, h, mips, usage, fmt, pool,
//                              filter, mipfilter, colorkey, pSrcInfo, pPalette, ppTexture)
void X_D3DXCreateTextureFromFileExA(X86 *c) {
    fails<13, -1, -1>(c, "D3DXCreateTextureFromFileExA", D3DXERR_INVALIDDATA_);
}
// D3DXCreateCubeTextureFromFileInMemory(device, src, len, ppCubeTexture)
void X_D3DXCreateCubeTextureFromFileInMemory(X86 *c) {
    fails<3, -1, -1>(c, "D3DXCreateCubeTextureFromFileInMemory", D3DXERR_INVALIDDATA_);
}
// D3DXCreateCubeTextureFromFileExA(device, file, size, mips, usage, fmt, pool,
//                                  filter, mipfilter, colorkey, pSrcInfo, pPalette, ppCubeTexture)
void X_D3DXCreateCubeTextureFromFileExA(X86 *c) {
    fails<12, -1, -1>(c, "D3DXCreateCubeTextureFromFileExA", D3DXERR_INVALIDDATA_);
}
// D3DXCreateVolumeTextureFromFileInMemory(device, src, len, ppVolumeTexture)
void X_D3DXCreateVolumeTextureFromFileInMemory(X86 *c) {
    fails<3, -1, -1>(c, "D3DXCreateVolumeTextureFromFileInMemory", D3DXERR_INVALIDDATA_);
}
// D3DXCreateVolumeTextureFromFileExA(device, file, w, h, d, mips, usage, fmt, pool,
//                                    filter, mipfilter, colorkey, pSrcInfo, pPalette, ppVolume)
void X_D3DXCreateVolumeTextureFromFileExA(X86 *c) {
    fails<14, -1, -1>(c, "D3DXCreateVolumeTextureFromFileExA", D3DXERR_INVALIDDATA_);
}
// D3DXGetImageInfoFromFileInMemory(src, len, pSrcInfo)
void X_D3DXGetImageInfoFromFileInMemory(X86 *c) {
    fails<-1, -1, -1>(c, "D3DXGetImageInfoFromFileInMemory", D3DXERR_INVALIDDATA_);
}
// D3DXLoadSurfaceFromSurface(dst, dstPal, dstRect, src, srcPal, srcRect, filter, colorkey)
void X_D3DXLoadSurfaceFromSurface(X86 *c) {
    fails<-1, -1, -1>(c, "D3DXLoadSurfaceFromSurface", D3DERR_NOTAVAILABLE_);
}
// D3DXSaveSurfaceToFileA(file, format, surface, palette, rect)
void X_D3DXSaveSurfaceToFileA(X86 *c) {
    fails<-1, -1, -1>(c, "D3DXSaveSurfaceToFileA", D3DERR_NOTAVAILABLE_);
}

// D3DXGetShaderVersion(pFunction): the version token, or 0.
void X_D3DXGetShaderVersion(X86 *c) {
    uint32_t f = arg(c, 0);
    set_eax(c, f && gm_valid(f, 4) ? rd32(f) : 0);
}

// The instruction length of a shader-model 2+ token, from its own size field
// (bits 24..27); comments carry theirs in bits 16..30.
bool next_token(uint32_t &p, uint32_t end) {
    uint32_t t = rd32(p);
    if ((t & 0xffffu) == 0xfffeu) { // comment
        p += 4u * (1u + ((t >> 16) & 0x7fffu));
        return p <= end;
    }
    p += 4u * (1u + ((t >> 24) & 0xfu));
    return p <= end;
}

// D3DXGetShaderInputSemantics(pFunction, pSemantics, pCount): the dcl
// instructions on input registers (v#) of a vs_2_0+ shader, as
// D3DXSEMANTIC { UINT Usage; UINT UsageIndex; }.
void X_D3DXGetShaderInputSemantics(X86 *c) {
    uint32_t f = arg(c, 0), out = arg(c, 1), count = arg(c, 2);
    if (!f || !gm_valid(f, 8)) {
        set_eax(c, D3DERR_INVALIDCALL_);
        return;
    }
    uint32_t version = rd32(f);
    uint32_t end = f + 4u * 65536u;
    std::vector<std::pair<uint32_t, uint32_t>> found;
    if ((version & 0xffff0000u) == 0xfffe0000u && (version & 0xff00u) >= 0x0200u) {
        for (uint32_t p = f + 4; gm_valid(p, 4) && p < end;) {
            uint32_t t = rd32(p);
            if (t == 0x0000ffffu)
                break;
            if ((t & 0xffffu) == 0x1fu && gm_valid(p, 12)) { // dcl
                uint32_t usage = rd32(p + 4), reg = rd32(p + 8);
                uint32_t type = ((reg >> 28) & 7u) | ((reg >> 8) & 0x18u);
                if (type == 1u) // D3DSPR_INPUT
                    found.emplace_back(usage & 0x1fu, (usage >> 16) & 0xfu);
            }
            if (!next_token(p, end))
                break;
        }
    }
    if (out)
        for (size_t i = 0; i < found.size(); ++i)
            if (gm_valid(out + 8u * (uint32_t)i, 8)) {
                wr32(out + 8u * (uint32_t)i, found[i].first);
                wr32(out + 8u * (uint32_t)i + 4, found[i].second);
            }
    if (count && gm_valid(count, 4))
        wr32(count, (uint32_t)found.size());
    set_eax(c, D3D_OK_);
}

uint32_t guest_string(const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1;
    uint32_t p = heap_alloc(n, true, 4);
    if (p)
        memcpy(gm_ptr(p), s, n);
    return p;
}
// D3DXGetVertexShaderProfile / D3DXGetPixelShaderProfile(device): the highest
// profile the device's caps report, a static string the caller does not free.
void X_D3DXGetVertexShaderProfile(X86 *c) {
    static uint32_t s = guest_string("vs_3_0");
    set_eax(c, s);
}
void X_D3DXGetPixelShaderProfile(X86 *c) {
    static uint32_t s = guest_string("ps_3_0");
    set_eax(c, s);
}

// D3DXCreateBuffer(NumBytes, ppBuffer): ID3DXBuffer is ID3DBlob's vtable.
void X_D3DXCreateBuffer(X86 *c) {
    uint32_t bytes = arg(c, 0), out = arg(c, 1);
    if (!out || !gm_valid(out, 4)) {
        set_eax(c, D3DERR_INVALIDCALL_);
        return;
    }
    wr32(out, 0);
    ComObj *obj = dx11::create(K_D3D_BLOB, IF_D3D_BLOB);
    dx11::Object *o = obj ? dx11::get(obj) : nullptr;
    if (!o) {
        set_eax(c, E_OUTOFMEMORY);
        return;
    }
    o->bytes = bytes;
    o->data = bytes ? heap_alloc(bytes, true, 16) : 0;
    if (bytes && !o->data) {
        com_release(obj);
        set_eax(c, E_OUTOFMEMORY);
        return;
    }
    wr32(out, com_view(obj, IF_D3D_BLOB));
    set_eax(c, D3D_OK_);
}

// D3DXCreateTexture(device, w, h, mips, usage, format, pool, ppTexture):
// D3DX_DEFAULT (0xffffffff) sizes become 256 and default mips a full chain.
void X_D3DXCreateTexture(X86 *c) {
    uint32_t device = arg(c, 0), w = arg(c, 1), h = arg(c, 2), mips = arg(c, 3);
    uint32_t out = arg(c, 7);
    if (!device || !gm_valid(device, 4) || !out) {
        set_eax(c, D3DERR_INVALIDCALL_);
        return;
    }
    zero_out(out);
    if (w == 0xffffffffu || w == 0)
        w = 256;
    if (h == 0xffffffffu || h == 0)
        h = 256;
    if (mips == 0xffffffffu)
        mips = 0;
    uint32_t vtbl = rd32(device);
    uint32_t create = rd32(vtbl + 4u * D3D_DEVICE9_CREATETEXTURE);
    uint32_t args[9] = {device, w, h, mips, arg(c, 4), arg(c, 5), arg(c, 6), out, 0};
    set_eax(c, guest_call(c, create, args, 9));
}

void write_m(uint32_t p, const float m[16]) {
    for (int i = 0; i < 16; ++i) {
        uint32_t b;
        memcpy(&b, &m[i], 4);
        wr32(p + 4u * (uint32_t)i, b);
    }
}
float rdf(uint32_t p) {
    uint32_t b = rd32(p);
    float f;
    memcpy(&f, &b, 4);
    return f;
}

// D3DXMatrixReflect(pOut, pPlane): reflection about the normalised plane.
void X_D3DXMatrixReflect(X86 *c) {
    uint32_t out = arg(c, 0), plane = arg(c, 1);
    float a = rdf(plane), b = rdf(plane + 4), cc = rdf(plane + 8), d = rdf(plane + 12);
    float len = sqrtf(a * a + b * b + cc * cc);
    if (len > 0.0f) {
        a /= len;
        b /= len;
        cc /= len;
        d /= len;
    }
    float m[16] = {-2 * a * a + 1, -2 * b * a,     -2 * cc * a,      0,
                   -2 * a * b,     -2 * b * b + 1, -2 * cc * b,      0,
                   -2 * a * cc,    -2 * b * cc,    -2 * cc * cc + 1, 0,
                   -2 * a * d,     -2 * b * d,     -2 * cc * d,      1};
    write_m(out, m);
    set_eax(c, out);
}

const ImportShim g_exports[] = {
    {"d3dx9_38.dll", "D3DXCreateEffect", 9, X_D3DXCreateEffect},
    {"d3dx9_38.dll", "D3DXCreateEffectCompiler", 7, X_D3DXCreateEffectCompiler},
    {"d3dx9_38.dll", "D3DXCompileShader", 10, X_D3DXCompileShader},
    {"d3dx9_38.dll", "D3DXCompileShaderFromFileA", 9, X_D3DXCompileShaderFromFileA},
    {"d3dx9_38.dll", "D3DXAssembleShader", 7, X_D3DXAssembleShader},
    {"d3dx9_38.dll", "D3DXAssembleShaderFromFileA", 6, X_D3DXAssembleShaderFromFileA},
    {"d3dx9_38.dll", "D3DXGetShaderConstantTable", 2, X_D3DXGetShaderConstantTable},
    {"d3dx9_38.dll", "D3DXGetShaderInputSemantics", 3, X_D3DXGetShaderInputSemantics},
    {"d3dx9_38.dll", "D3DXGetShaderVersion", 1, X_D3DXGetShaderVersion},
    {"d3dx9_38.dll", "D3DXGetVertexShaderProfile", 1, X_D3DXGetVertexShaderProfile},
    {"d3dx9_38.dll", "D3DXGetPixelShaderProfile", 1, X_D3DXGetPixelShaderProfile},
    {"d3dx9_38.dll", "D3DXCreateBuffer", 2, X_D3DXCreateBuffer},
    {"d3dx9_38.dll", "D3DXCreateTexture", 8, X_D3DXCreateTexture},
    {"d3dx9_38.dll", "D3DXCreateTextureFromFileInMemory", 4, X_D3DXCreateTextureFromFileInMemory},
    {"d3dx9_38.dll", "D3DXCreateTextureFromFileExA", 14, X_D3DXCreateTextureFromFileExA},
    {"d3dx9_38.dll", "D3DXCreateCubeTextureFromFileInMemory", 4,
     X_D3DXCreateCubeTextureFromFileInMemory},
    {"d3dx9_38.dll", "D3DXCreateCubeTextureFromFileExA", 13, X_D3DXCreateCubeTextureFromFileExA},
    {"d3dx9_38.dll", "D3DXCreateVolumeTextureFromFileInMemory", 4,
     X_D3DXCreateVolumeTextureFromFileInMemory},
    {"d3dx9_38.dll", "D3DXCreateVolumeTextureFromFileExA", 15,
     X_D3DXCreateVolumeTextureFromFileExA},
    {"d3dx9_38.dll", "D3DXGetImageInfoFromFileInMemory", 3, X_D3DXGetImageInfoFromFileInMemory},
    {"d3dx9_38.dll", "D3DXLoadSurfaceFromSurface", 8, X_D3DXLoadSurfaceFromSurface},
    {"d3dx9_38.dll", "D3DXSaveSurfaceToFileA", 5, X_D3DXSaveSurfaceToFileA},
    {"d3dx9_38.dll", "D3DXMatrixReflect", 2, X_D3DXMatrixReflect},
};

} // namespace

// Registers the d3dx9_26 table again under d3dx9_38.dll, then the exports only
// the newer DLL has here.
void d3dx9_38_register(const ImportShim *d3dx9_26, size_t count) {
    static std::vector<ImportShim> aliases;
    if (aliases.empty())
        for (size_t i = 0; i < count; ++i) {
            ImportShim s = d3dx9_26[i];
            s.dll = "d3dx9_38.dll";
            aliases.push_back(s);
        }
    imports_register(aliases.data(), aliases.size());
    imports_register(g_exports, std::size(g_exports));
    // IID_ID3DXEffect as D3DX 9.36 and later name it,
    // {F6CEB4B3-4E4C-40DD-B883-8D8DE5EA0CD5}; the effect's vtable is already
    // that version's (DeleteParameterBlock and SetRawValue included).
    static const uint8_t IID_ID3DXEffect_38[16] = {0xb3, 0xb4, 0xce, 0xf6, 0x4c, 0x4e, 0xdd, 0x40,
                                                   0xb8, 0x83, 0x8d, 0x8d, 0xe5, 0xea, 0x0c, 0xd5};
    com_register_iid(IF_D3DXEFFECT, IID_ID3DXEffect_38);
}
