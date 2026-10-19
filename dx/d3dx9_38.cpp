// d3dx9_38.cpp - D3DX 9 as a March 2008 SDK game links it (d3dx9_38.dll).
//
// Every d3dx9_26 export the kit implements keeps its behaviour under the newer
// DLL name; the interfaces did not change between the two. On top of that a
// Gamebryo-era game calls the rest of this list (Fallout: New Vegas first;
// the shader utilities and D3DXCreateBuffer follow giggity-bully's version):
//
//  - Textures from DDS files in memory, 2D, cube and volume: the DDS header
//    picks the Direct3D format and the level count, the device's own
//    CreateTexture / CreateCubeTexture / CreateVolumeTexture make the
//    texture (called through its vtable, so it is an ordinary device
//    texture in the managed pool), and each level is copied in through
//    LockRect / LockBox. D3DXGetImageInfoFromFileInMemory reads the same
//    header. Other image file formats fail cleanly.
//  - The shader utilities: version and input-semantic queries read the
//    bytecode itself; the profile queries name what the device's caps report
//    (vs_3_0 / ps_3_0).
//  - D3DXCreateBuffer: an ID3DXBuffer, which has ID3DBlob's exact vtable.
//  - Matrix, vector and plane maths: real maths.
//
// What fails cleanly, each reported once by name: run-time HLSL compiling and
// assembling, the shader constant table, surface-to-surface copies and
// saving textures to files.
#include "com.h"
#include "d3d11.h"
#include "dx.h"
#include "../runtime/guest.h"
#include "../runtime/imports.h"
#include "../runtime/memory.h"

#include <math.h>
#include <string.h>
#include <iterator>
#include <string>
#include <vector>

namespace {

const uint32_t D3D_OK_ = 0;
const uint32_t D3DERR_INVALIDCALL_ = 0x8876086cu;
const uint32_t D3DERR_NOTAVAILABLE_ = 0x8876086au;
const uint32_t D3DXERR_INVALIDDATA_ = 0x88760b59u;
const uint32_t E_FAIL_ = 0x80004005u;
const uint32_t E_OUTOFMEMORY_ = 0x8007000eu;

// IDirect3DDevice9 and texture vtable slots.
const uint32_t DEV_CREATETEXTURE = 23, DEV_CREATEVOLUMETEXTURE = 24, DEV_CREATECUBETEXTURE = 25;
const uint32_t TEX_LOCK = 19, TEX_UNLOCK = 20, IUNKNOWN_RELEASE = 2;
const uint32_t D3DPOOL_MANAGED = 1;

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

uint32_t vcall(X86 *c, uint32_t obj, uint32_t slot, std::vector<uint32_t> args) {
    uint32_t fn = rd32(rd32(obj) + 4u * slot);
    args.insert(args.begin(), obj);
    return guest_call(c, fn, args.data(), (int)args.size());
}

// ---------------------------------------------------------------- DDS ----

uint32_t fourcc(const char *s) {
    return (uint32_t)(uint8_t)s[0] | ((uint32_t)(uint8_t)s[1] << 8) |
           ((uint32_t)(uint8_t)s[2] << 16) | ((uint32_t)(uint8_t)s[3] << 24);
}

struct DdsInfo {
    uint32_t width = 0, height = 0, depth = 1, levels = 1;
    uint32_t format = 0; // D3DFORMAT
    bool cube = false, volume = false;
    bool block = false;    // 4x4 compressed blocks
    uint32_t unit = 0;     // bytes per block, or bytes per pixel
    bool expand24 = false; // R8G8B8 in the file, X8R8G8B8 on the device
    uint32_t data = 0;     // offset of the first level in the file
    uint32_t type = 3;     // D3DRESOURCETYPE: texture 3, volume 4, cube 5
};

// Maps a DDS pixel format onto the D3DFORMAT D3DX would create.
bool dds_parse(const uint8_t *p, uint32_t len, DdsInfo &d) {
    auto u32 = [p](uint32_t at) {
        uint32_t v;
        memcpy(&v, p + at, 4);
        return v;
    };
    if (len < 128 || u32(0) != fourcc("DDS ") || u32(4) != 124)
        return false;
    d.height = u32(12);
    d.width = u32(16);
    uint32_t flags = u32(8);
    d.depth = (flags & 0x800000u) && u32(24) ? u32(24) : 1;
    d.levels = u32(28) ? u32(28) : 1;
    uint32_t pf_flags = u32(80), cc = u32(84), bits = u32(88);
    uint32_t rm = u32(92), gm = u32(96), bm = u32(100), am = u32(104);
    uint32_t caps2 = u32(112);
    d.cube = (caps2 & 0x200u) != 0;
    d.volume = (caps2 & 0x200000u) != 0 && d.depth > 1;
    d.type = d.cube ? 5 : d.volume ? 4 : 3;
    d.data = 128;
    if (pf_flags & 0x4u) { // DDPF_FOURCC
        if (cc == fourcc("DXT1")) {
            d.format = cc, d.block = true, d.unit = 8;
        } else if (cc == fourcc("DXT2") || cc == fourcc("DXT3") || cc == fourcc("DXT4") ||
                   cc == fourcc("DXT5")) {
            d.format = cc, d.block = true, d.unit = 16;
        } else if (cc == fourcc("ATI1")) {
            d.format = cc, d.block = true, d.unit = 8;
        } else if (cc == fourcc("ATI2")) {
            d.format = cc, d.block = true, d.unit = 16;
        } else {
            // A D3DFORMAT number in the FourCC field (float formats).
            switch (cc) {
            case 36:
                d.format = cc, d.unit = 8;
                break; // A16B16G16R16
            case 111:
                d.format = cc, d.unit = 2;
                break; // R16F
            case 112:
                d.format = cc, d.unit = 4;
                break; // G16R16F
            case 113:
                d.format = cc, d.unit = 8;
                break; // A16B16G16R16F
            case 114:
                d.format = cc, d.unit = 4;
                break; // R32F
            case 115:
                d.format = cc, d.unit = 8;
                break; // G32R32F
            case 116:
                d.format = cc, d.unit = 16;
                break; // A32B32G32R32F
            default:
                return false;
            }
        }
    } else if (pf_flags & 0x40u) { // DDPF_RGB
        bool alpha = (pf_flags & 0x1u) && am;
        if (bits == 32) {
            d.unit = 4;
            if (rm == 0xff0000u && gm == 0xff00u && bm == 0xffu)
                d.format = alpha ? 21 : 22; // A8R8G8B8 / X8R8G8B8
            else if (rm == 0xffu && gm == 0xff00u && bm == 0xff0000u)
                d.format = alpha ? 32 : 33; // A8B8G8R8 / X8B8G8R8
            else if (rm == 0xffffu && gm == 0xffff0000u)
                d.format = 34; // G16R16
            else
                return false;
        } else if (bits == 24) {
            d.unit = 3, d.format = 22, d.expand24 = true;
        } else if (bits == 16) {
            d.unit = 2;
            if (rm == 0xf800u)
                d.format = 23; // R5G6B5
            else if (rm == 0x7c00u)
                d.format = alpha ? 25 : 24; // A1R5G5B5 / X1R5G5B5
            else if (rm == 0x0f00u)
                d.format = alpha ? 26 : 30; // A4R4G4B4 / X4R4G4B4
            else
                return false;
        } else {
            return false;
        }
    } else if (pf_flags & 0x20000u) { // DDPF_LUMINANCE
        if (bits == 8)
            d.unit = 1, d.format = 50; // L8
        else if (bits == 16 && am)
            d.unit = 2, d.format = 51; // A8L8
        else if (bits == 16)
            d.unit = 2, d.format = 81; // L16
        else
            return false;
    } else if ((pf_flags & 0x2u) && bits == 8) {      // DDPF_ALPHA
        d.unit = 1, d.format = 28;                    // A8
    } else if ((pf_flags & 0x80000u) && bits == 16) { // DDPF_BUMPDUDV
        d.unit = 2, d.format = 60;                    // V8U8
    } else {
        return false;
    }
    if (!d.width || !d.height)
        return false;
    // A level count past the 1x1 level is a broken header.
    uint32_t max_levels = 1;
    for (uint32_t s = d.width > d.height ? d.width : d.height; s > 1; s >>= 1)
        ++max_levels;
    if (d.levels > max_levels)
        d.levels = max_levels;
    return true;
}

uint32_t level_dim(uint32_t v, uint32_t level) {
    v >>= level;
    return v ? v : 1;
}
// Bytes of one row (of blocks, for a compressed format) and of rows in a level.
void level_shape(const DdsInfo &d, uint32_t level, uint32_t &row_bytes, uint32_t &rows) {
    uint32_t w = level_dim(d.width, level), h = level_dim(d.height, level);
    if (d.block) {
        row_bytes = ((w + 3) / 4) * d.unit;
        rows = (h + 3) / 4;
    } else {
        row_bytes = w * d.unit;
        rows = h;
    }
}

// Copies one level (or one slice of it) into a locked rectangle.
void copy_rows(const uint8_t *src, uint32_t row_bytes, uint32_t rows, uint32_t dst, uint32_t pitch,
               bool expand24, uint32_t width) {
    for (uint32_t r = 0; r < rows; ++r) {
        uint32_t to = dst + r * pitch;
        const uint8_t *from = src + (size_t)r * row_bytes;
        if (expand24) {
            if (!gm_valid(to, width * 4))
                return;
            uint8_t *o = g_mem + to;
            for (uint32_t x = 0; x < width; ++x) {
                o[4 * x + 0] = from[3 * x + 0];
                o[4 * x + 1] = from[3 * x + 1];
                o[4 * x + 2] = from[3 * x + 2];
                o[4 * x + 3] = 0xff;
            }
        } else {
            uint32_t n = row_bytes < pitch || !pitch ? row_bytes : pitch;
            if (!gm_valid(to, n))
                return;
            memcpy(g_mem + to, from, n);
        }
    }
}

uint32_t level_bytes(const DdsInfo &d, uint32_t level) {
    uint32_t row_bytes, rows;
    level_shape(d, level, row_bytes, rows);
    return row_bytes * rows * (d.volume ? level_dim(d.depth, level) : 1);
}

// The file bytes the header promises, checked before anything is created.
bool dds_complete(const DdsInfo &d, uint32_t len) {
    uint64_t need = d.data;
    for (uint32_t l = 0; l < d.levels; ++l)
        need += level_bytes(d, l);
    if (d.cube)
        need = d.data + (need - d.data) * 6;
    return need <= len;
}

// (device, pSrcData, SrcDataSize, ppTexture) for each of the three kinds.
void create_from_dds(X86 *c, uint32_t want_type) {
    uint32_t device = arg(c, 0), src = arg(c, 1), len = arg(c, 2), out = arg(c, 3);
    zero_out(out);
    if (!device || !src || !len || !out || !gm_valid(src, len) || !gm_valid(device, 4)) {
        set_eax(c, D3DERR_INVALIDCALL_);
        return;
    }
    std::vector<uint8_t> file(g_mem + src, g_mem + src + len); // creation may move nothing, but
                                                               // the guest may free src early
    DdsInfo d;
    if (!dds_parse(file.data(), len, d)) {
        char what[96];
        snprintf(what, sizeof what, "a texture file that is not a supported DDS (%02x%02x%02x%02x)",
                 file[0], len > 1 ? file[1] : 0, len > 2 ? file[2] : 0, len > 3 ? file[3] : 0);
        report_once(what);
        set_eax(c, D3DXERR_INVALIDDATA_);
        return;
    }
    if (!dds_complete(d, len)) {
        LOGW("d3dx9_38: DDS %ux%u with %u levels is truncated (%u bytes)", d.width, d.height,
             d.levels, len);
        set_eax(c, D3DXERR_INVALIDDATA_);
        return;
    }
    // A 2D load of a cube or volume file uses its first face or slice; a cube
    // load of a 2D file is invalid, as D3DX has it.
    if (want_type == 5 && !d.cube) {
        set_eax(c, D3DXERR_INVALIDDATA_);
        return;
    }
    uint32_t tmp = heap_alloc(16, true, 4); // the out pointer, then D3DLOCKED_RECT/BOX
    if (!tmp) {
        set_eax(c, E_OUTOFMEMORY_);
        return;
    }
    uint32_t hr;
    if (want_type == 5)
        hr = vcall(c, device, DEV_CREATECUBETEXTURE,
                   {d.width, d.levels, 0, d.format, D3DPOOL_MANAGED, tmp, 0});
    else if (want_type == 4)
        hr = vcall(c, device, DEV_CREATEVOLUMETEXTURE,
                   {d.width, d.height, d.depth, d.levels, 0, d.format, D3DPOOL_MANAGED, tmp, 0});
    else
        hr = vcall(c, device, DEV_CREATETEXTURE,
                   {d.width, d.height, d.levels, 0, d.format, D3DPOOL_MANAGED, tmp, 0});
    uint32_t tex = rd32(tmp);
    if (hr != D3D_OK_ || !tex) {
        LOGW("d3dx9_38: device refused a %ux%u texture, format %08x, %u levels: %08x", d.width,
             d.height, d.format, d.levels, hr);
        heap_free(tmp);
        set_eax(c, hr ? hr : E_FAIL_);
        return;
    }
    uint32_t lock = tmp + 4;
    const uint8_t *at = file.data() + d.data;
    uint32_t faces = d.cube ? 6 : 1;
    for (uint32_t face = 0; face < faces; ++face)
        for (uint32_t level = 0; level < d.levels; ++level) {
            uint32_t row_bytes, rows;
            level_shape(d, level, row_bytes, rows);
            uint32_t w = level_dim(d.width, level);
            uint32_t slices = d.volume ? level_dim(d.depth, level) : 1;
            bool want = want_type == 5 || face == 0;
            if (want && want_type == 4) {
                // LockBox(level, D3DLOCKED_BOX *, box, flags): RowPitch, SlicePitch, pBits.
                if (vcall(c, tex, TEX_LOCK, {level, lock, 0, 0}) == D3D_OK_) {
                    uint32_t row_pitch = rd32(lock), slice_pitch = rd32(lock + 4),
                             bits = rd32(lock + 8);
                    for (uint32_t s = 0; s < slices; ++s)
                        copy_rows(at + (size_t)s * row_bytes * rows, row_bytes, rows,
                                  bits + s * slice_pitch, row_pitch, d.expand24, w);
                    vcall(c, tex, TEX_UNLOCK, {level});
                }
            } else if (want) {
                // LockRect([face,] level, D3DLOCKED_RECT *, rect, flags): Pitch, pBits.
                uint32_t r = want_type == 5 ? vcall(c, tex, TEX_LOCK, {face, level, lock, 0, 0})
                                            : vcall(c, tex, TEX_LOCK, {level, lock, 0, 0});
                if (r == D3D_OK_) {
                    copy_rows(at, row_bytes, rows, rd32(lock + 4), rd32(lock), d.expand24, w);
                    if (want_type == 5)
                        vcall(c, tex, TEX_UNLOCK, {face, level});
                    else
                        vcall(c, tex, TEX_UNLOCK, {level});
                }
            }
            at += (size_t)row_bytes * rows * slices;
        }
    heap_free(tmp);
    wr32(out, tex);
    set_eax(c, D3D_OK_);
}

// D3DXCreateTextureFromFileInMemory(device, src, len, ppTexture)
void X_D3DXCreateTextureFromFileInMemory(X86 *c) {
    create_from_dds(c, 3);
}
// D3DXCreateCubeTextureFromFileInMemory(device, src, len, ppCubeTexture)
void X_D3DXCreateCubeTextureFromFileInMemory(X86 *c) {
    create_from_dds(c, 5);
}
// D3DXCreateVolumeTextureFromFileInMemory(device, src, len, ppVolumeTexture)
void X_D3DXCreateVolumeTextureFromFileInMemory(X86 *c) {
    create_from_dds(c, 4);
}

// D3DXGetImageInfoFromFileInMemory(src, len, D3DXIMAGE_INFO *): Width,
// Height, Depth, MipLevels, Format, ResourceType, ImageFileFormat (DDS = 4).
void X_D3DXGetImageInfoFromFileInMemory(X86 *c) {
    uint32_t src = arg(c, 0), len = arg(c, 1), out = arg(c, 2);
    DdsInfo d;
    if (!src || !len || !gm_valid(src, len) || !dds_parse(g_mem + src, len, d)) {
        set_eax(c, D3DXERR_INVALIDDATA_);
        return;
    }
    if (out && gm_valid(out, 28)) {
        wr32(out + 0, d.width);
        wr32(out + 4, d.height);
        wr32(out + 8, d.depth);
        wr32(out + 12, d.levels);
        wr32(out + 16, d.format);
        wr32(out + 20, d.type);
        wr32(out + 24, 4);
    }
    set_eax(c, D3D_OK_);
}

// --------------------------------------------------------- shaders ----

// D3DXCompileShader(src, len, defines, include, function, profile, flags,
//                   ppShader, ppErrorMsgs, ppConstantTable)
void X_D3DXCompileShader(X86 *c) {
    fails<7, 8, 9>(c, "D3DXCompileShader", E_FAIL_);
}
// D3DXCompileShaderFromFileA(file, defines, include, function, profile, flags,
//                            ppShader, ppErrorMsgs, ppConstantTable)
void X_D3DXCompileShaderFromFileA(X86 *c) {
    fails<6, 7, 8>(c, "D3DXCompileShaderFromFileA", E_FAIL_);
}
// D3DXAssembleShader(src, len, defines, include, flags, ppShader, ppErrorMsgs)
void X_D3DXAssembleShader(X86 *c) {
    fails<5, 6, -1>(c, "D3DXAssembleShader", E_FAIL_);
}
// D3DXAssembleShaderFromFileA(file, defines, include, flags, ppShader, ppErrorMsgs)
void X_D3DXAssembleShaderFromFileA(X86 *c) {
    fails<4, 5, -1>(c, "D3DXAssembleShaderFromFileA", E_FAIL_);
}
// D3DXGetShaderConstantTable(pFunction, ppConstantTable)
void X_D3DXGetShaderConstantTable(X86 *c) {
    fails<1, -1, -1>(c, "D3DXGetShaderConstantTable", D3DXERR_INVALIDDATA_);
}
// D3DXLoadSurfaceFromSurface(dst, dstPal, dstRect, src, srcPal, srcRect, filter, colorkey)
void X_D3DXLoadSurfaceFromSurface(X86 *c) {
    fails<-1, -1, -1>(c, "D3DXLoadSurfaceFromSurface", D3DERR_NOTAVAILABLE_);
}
// D3DXSaveTextureToFileA(file, format, texture, palette)
void X_D3DXSaveTextureToFileA(X86 *c) {
    fails<-1, -1, -1>(c, "D3DXSaveTextureToFileA", D3DERR_NOTAVAILABLE_);
}

// D3DXGetShaderVersion(pFunction): the version token, or 0.
void X_D3DXGetShaderVersion(X86 *c) {
    uint32_t f = arg(c, 0);
    set_eax(c, f && gm_valid(f, 4) ? rd32(f) : 0);
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
        set_eax(c, E_OUTOFMEMORY_);
        return;
    }
    o->bytes = bytes;
    o->data = bytes ? heap_alloc(bytes, true, 16) : 0;
    if (bytes && !o->data) {
        com_release(obj);
        set_eax(c, E_OUTOFMEMORY_);
        return;
    }
    wr32(out, com_view(obj, IF_D3D_BLOB));
    set_eax(c, D3D_OK_);
}

// ------------------------------------------------------------ maths ----

float rdf(uint32_t p) {
    uint32_t b = rd32(p);
    float f;
    memcpy(&f, &b, 4);
    return f;
}
void wrf(uint32_t p, float f) {
    uint32_t b;
    memcpy(&b, &f, 4);
    wr32(p, b);
}
void read_m(uint32_t p, float m[16]) {
    for (int i = 0; i < 16; ++i)
        m[i] = rdf(p + 4u * (uint32_t)i);
}
void write_m(uint32_t p, const float m[16]) {
    for (int i = 0; i < 16; ++i)
        wrf(p + 4u * (uint32_t)i, m[i]);
}
void mul_m(const float a[16], const float b[16], float r[16]) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r[4 * i + j] = a[4 * i] * b[j] + a[4 * i + 1] * b[4 + j] + a[4 * i + 2] * b[8 + j] +
                           a[4 * i + 3] * b[12 + j];
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

// D3DXMatrixRotationYawPitchRoll(pOut, yaw, pitch, roll): roll about Z, then
// pitch about X, then yaw about Y (row vectors).
void X_D3DXMatrixRotationYawPitchRoll(X86 *c) {
    uint32_t out = arg(c, 0);
    auto as_float = [](uint32_t bits) {
        float f;
        memcpy(&f, &bits, 4);
        return f;
    };
    float yaw = as_float(arg(c, 1)), pitch = as_float(arg(c, 2)), roll = as_float(arg(c, 3));
    float sy = sinf(yaw), cy = cosf(yaw), sp = sinf(pitch), cp = cosf(pitch);
    float sr = sinf(roll), cr = cosf(roll);
    float m[16] = {cr * cy + sr * sp * sy,
                   sr * cp,
                   sr * sp * cy - cr * sy,
                   0,
                   cr * sp * sy - sr * cy,
                   cr * cp,
                   sr * sy + cr * sp * cy,
                   0,
                   cp * sy,
                   -sp,
                   cp * cy,
                   0,
                   0,
                   0,
                   0,
                   1};
    write_m(out, m);
    set_eax(c, out);
}

// D3DXMatrixMultiplyTranspose(pOut, pM1, pM2): transpose(M1 * M2).
void X_D3DXMatrixMultiplyTranspose(X86 *c) {
    uint32_t out = arg(c, 0);
    float a[16], b[16], r[16], t[16];
    read_m(arg(c, 1), a);
    read_m(arg(c, 2), b);
    mul_m(a, b, r);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            t[4 * j + i] = r[4 * i + j];
    write_m(out, t);
    set_eax(c, out);
}

// D3DXVec3TransformCoord(pOut, pV, pM): (x, y, z, 1) * M, divided by w.
void X_D3DXVec3TransformCoord(X86 *c) {
    uint32_t out = arg(c, 0), v = arg(c, 1);
    float m[16];
    read_m(arg(c, 2), m);
    float x = rdf(v), y = rdf(v + 4), z = rdf(v + 8), r[4];
    for (int j = 0; j < 4; ++j)
        r[j] = x * m[j] + y * m[4 + j] + z * m[8 + j] + m[12 + j];
    float w = r[3] == 0.0f ? 1.0f : 1.0f / r[3];
    wrf(out, r[0] * w);
    wrf(out + 4, r[1] * w);
    wrf(out + 8, r[2] * w);
    set_eax(c, out);
}

// D3DXPlaneNormalize(pOut, pP): the plane scaled to a unit normal.
void X_D3DXPlaneNormalize(X86 *c) {
    uint32_t out = arg(c, 0), p = arg(c, 1);
    float a = rdf(p), b = rdf(p + 4), cc = rdf(p + 8), d = rdf(p + 12);
    float len = sqrtf(a * a + b * b + cc * cc);
    float s = len > 0.0f ? 1.0f / len : 0.0f;
    wrf(out, a * s);
    wrf(out + 4, b * s);
    wrf(out + 8, cc * s);
    wrf(out + 12, d * s);
    set_eax(c, out);
}

// D3DXPlaneTransform(pOut, pP, pM): the plane as a row vector times M; the
// caller passes the inverse transpose of the point transform, as in D3DX.
void X_D3DXPlaneTransform(X86 *c) {
    uint32_t out = arg(c, 0), p = arg(c, 1);
    float m[16];
    read_m(arg(c, 2), m);
    float v[4] = {rdf(p), rdf(p + 4), rdf(p + 8), rdf(p + 12)}, r[4];
    for (int j = 0; j < 4; ++j)
        r[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j] + v[3] * m[12 + j];
    for (int j = 0; j < 4; ++j)
        wrf(out + 4u * (uint32_t)j, r[j]);
    set_eax(c, out);
}

const ImportShim g_exports[] = {
    {"d3dx9_38.dll", "D3DXCompileShader", 10, X_D3DXCompileShader},
    {"d3dx9_38.dll", "D3DXCompileShaderFromFileA", 9, X_D3DXCompileShaderFromFileA},
    {"d3dx9_38.dll", "D3DXAssembleShader", 7, X_D3DXAssembleShader},
    {"d3dx9_38.dll", "D3DXAssembleShaderFromFileA", 6, X_D3DXAssembleShaderFromFileA},
    {"d3dx9_38.dll", "D3DXGetShaderConstantTable", 2, X_D3DXGetShaderConstantTable},
    {"d3dx9_38.dll", "D3DXGetShaderVersion", 1, X_D3DXGetShaderVersion},
    {"d3dx9_38.dll", "D3DXGetVertexShaderProfile", 1, X_D3DXGetVertexShaderProfile},
    {"d3dx9_38.dll", "D3DXGetPixelShaderProfile", 1, X_D3DXGetPixelShaderProfile},
    {"d3dx9_38.dll", "D3DXCreateBuffer", 2, X_D3DXCreateBuffer},
    {"d3dx9_38.dll", "D3DXCreateTextureFromFileInMemory", 4, X_D3DXCreateTextureFromFileInMemory},
    {"d3dx9_38.dll", "D3DXCreateCubeTextureFromFileInMemory", 4,
     X_D3DXCreateCubeTextureFromFileInMemory},
    {"d3dx9_38.dll", "D3DXCreateVolumeTextureFromFileInMemory", 4,
     X_D3DXCreateVolumeTextureFromFileInMemory},
    {"d3dx9_38.dll", "D3DXGetImageInfoFromFileInMemory", 3, X_D3DXGetImageInfoFromFileInMemory},
    {"d3dx9_38.dll", "D3DXLoadSurfaceFromSurface", 8, X_D3DXLoadSurfaceFromSurface},
    {"d3dx9_38.dll", "D3DXSaveTextureToFileA", 4, X_D3DXSaveTextureToFileA},
    {"d3dx9_38.dll", "D3DXMatrixReflect", 2, X_D3DXMatrixReflect},
    {"d3dx9_38.dll", "D3DXMatrixRotationYawPitchRoll", 4, X_D3DXMatrixRotationYawPitchRoll},
    {"d3dx9_38.dll", "D3DXMatrixMultiplyTranspose", 3, X_D3DXMatrixMultiplyTranspose},
    {"d3dx9_38.dll", "D3DXVec3TransformCoord", 3, X_D3DXVec3TransformCoord},
    {"d3dx9_38.dll", "D3DXPlaneNormalize", 2, X_D3DXPlaneNormalize},
    {"d3dx9_38.dll", "D3DXPlaneTransform", 3, X_D3DXPlaneTransform},
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
