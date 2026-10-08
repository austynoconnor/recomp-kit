// d3d8.cpp - Direct3D 8, as views of the Direct3D 9 objects.
//
// Direct3D 8 and 9 share their object model: a factory, a device, textures,
// surfaces and buffers with the same lifetimes and nearly the same methods.
// So version 8 here is not a second renderer. Every Direct3D 8 interface is
// one more view (com.h) of a d3d9.cpp object, with a vtable in the version 8
// slot order:
//
//   - A slot whose arguments match version 9 points straight at the version
//     9 method.
//   - A slot whose arguments differ rewrites them and calls the version 9
//     method through shim_forward (imports.h), then turns the interface
//     pointers it hands back into version 8 views.
//   - What version 8 has alone - vertex shader handles with their own
//     declaration format, integer state block tokens, CopyRects, the
//     texture-stage sampler states - is implemented here.
//
// The structures that changed between the versions (D3DPRESENT_PARAMETERS,
// D3DCAPS8, D3DADAPTER_IDENTIFIER8, the surface and buffer descriptions) are
// converted on the way through. Drawing, including the fixed-function
// pipeline (d3d9_ffp.cpp), is the version 9 device's. Every game-specific
// choice stays out of this file: it serves any Direct3D 8 game.
//
// Written for the kit from the DirectX 8 SDK documentation of the interfaces;
// the approach is the one d3d8to9 popularised, but no code was taken from it.
#include "com.h"
#include "dx.h"
#include "d3d9_pipeline.h"
#include "../runtime/guest.h"
#include "../runtime/memory.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <map>
#include <memory>
#include <string.h>
#include <vector>

#define IID_BYTES8(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7)                                        \
    {(uint8_t)((a) & 0xff),                                                                        \
     (uint8_t)(((a) >> 8) & 0xff),                                                                 \
     (uint8_t)(((a) >> 16) & 0xff),                                                                \
     (uint8_t)(((a) >> 24) & 0xff),                                                                \
     (uint8_t)((b) & 0xff),                                                                        \
     (uint8_t)(((b) >> 8) & 0xff),                                                                 \
     (uint8_t)((c) & 0xff),                                                                        \
     (uint8_t)(((c) >> 8) & 0xff),                                                                 \
     d0,                                                                                           \
     d1,                                                                                           \
     d2,                                                                                           \
     d3,                                                                                           \
     d4,                                                                                           \
     d5,                                                                                           \
     d6,                                                                                           \
     d7}

static const uint8_t IID_IDirect3D8_[16] =
    IID_BYTES8(0x1dd9e8da, 0x1c77, 0x4d40, 0xb0, 0xcf, 0x98, 0xfe, 0xfd, 0xff, 0x95, 0x12);
static const uint8_t IID_IDirect3DDevice8_[16] =
    IID_BYTES8(0x7385e5df, 0x8fe8, 0x41d5, 0x86, 0xb6, 0xd7, 0xb4, 0x85, 0x47, 0xb6, 0xcf);
static const uint8_t IID_IDirect3DTexture8_[16] =
    IID_BYTES8(0xe4cdd575, 0x2866, 0x4f01, 0xb1, 0x2e, 0x7e, 0xec, 0xe1, 0xec, 0x93, 0x58);
static const uint8_t IID_IDirect3DCubeTexture8_[16] =
    IID_BYTES8(0x3ee5b968, 0x2aca, 0x4c34, 0x8b, 0xb5, 0x7e, 0x0c, 0x3d, 0x19, 0xb7, 0x50);
static const uint8_t IID_IDirect3DVertexBuffer8_[16] =
    IID_BYTES8(0x8aeeeac7, 0x05f9, 0x44d4, 0xb5, 0x91, 0x00, 0x0b, 0x0d, 0xf1, 0xcb, 0x95);
static const uint8_t IID_IDirect3DIndexBuffer8_[16] =
    IID_BYTES8(0x0e689c9a, 0x053d, 0x44a0, 0x9d, 0x92, 0xdb, 0x0e, 0x3d, 0x75, 0x0f, 0x86);
static const uint8_t IID_IDirect3DSurface8_[16] =
    IID_BYTES8(0xb96eebca, 0xb326, 0x4ea5, 0x88, 0x2f, 0x2f, 0xf5, 0xba, 0xe0, 0x21, 0xdd);

static const uint32_t D3D_OK8 = 0;
static const uint32_t D3DERR_INVALIDCALL8 = 0x8876086cu;
static const uint32_t D3DERR_NOTAVAILABLE8 = 0x8876086au;
static const uint32_t D3DERR_NOTFOUND8 = 0x88760866u;

// ---------------------------------------------------------------------------
// The version 9 methods the version 8 slots reach (d3d9.cpp).
// ---------------------------------------------------------------------------
#define D9_METHODS(X)                                                                              \
    X(D9_RegisterSoftwareDevice)                                                                   \
    X(D9_GetAdapterCount)                                                                          \
    X(D9_GetAdapterIdentifier)                                                                     \
    X(D9_GetAdapterDisplayMode)                                                                    \
    X(D9_CheckDeviceType)                                                                          \
    X(D9_CheckDeviceFormat)                                                                        \
    X(D9_CheckDeviceMultiSampleType)                                                               \
    X(D9_CheckDepthStencilMatch)                                                                   \
    X(D9_GetDeviceCaps)                                                                            \
    X(D9_GetAdapterMonitor)                                                                        \
    X(D9_CreateDevice)                                                                             \
    X(Dev_TestCooperativeLevel)                                                                    \
    X(Dev_GetAvailableTextureMem)                                                                  \
    X(Dev_GetDirect3D)                                                                             \
    X(Dev_GetDeviceCaps)                                                                           \
    X(Dev_SetCursorProperties)                                                                     \
    X(Dev_SetCursorPosition)                                                                       \
    X(Dev_ShowCursor)                                                                              \
    X(Dev_Reset)                                                                                   \
    X(Dev_Present)                                                                                 \
    X(Dev_GetBackBuffer)                                                                           \
    X(Dev_CreateTexture)                                                                           \
    X(Dev_CreateCubeTexture)                                                                       \
    X(Dev_CreateVertexBuffer)                                                                      \
    X(Dev_CreateIndexBuffer)                                                                       \
    X(Dev_CreateRenderTarget)                                                                      \
    X(Dev_CreateDepthStencilSurface)                                                               \
    X(Dev_CreateOffscreenPlainSurface)                                                             \
    X(Dev_SetRenderTarget)                                                                         \
    X(Dev_SetDepthStencilSurface)                                                                  \
    X(Dev_GetDepthStencilSurface)                                                                  \
    X(Dev_BeginScene)                                                                              \
    X(Dev_EndScene)                                                                                \
    X(Dev_Clear)                                                                                   \
    X(Dev_SetTransform)                                                                            \
    X(Dev_GetTransform)                                                                            \
    X(Dev_MultiplyTransform)                                                                       \
    X(Dev_SetViewport)                                                                             \
    X(Dev_GetViewport)                                                                             \
    X(Dev_SetMaterial)                                                                             \
    X(Dev_GetMaterial)                                                                             \
    X(Dev_SetLight)                                                                                \
    X(Dev_GetLight)                                                                                \
    X(Dev_LightEnable)                                                                             \
    X(Dev_GetLightEnable)                                                                          \
    X(Dev_SetClipPlane)                                                                            \
    X(Dev_GetClipPlane)                                                                            \
    X(Dev_SetRenderState)                                                                          \
    X(Dev_GetRenderState)                                                                          \
    X(Dev_SetClipStatus)                                                                           \
    X(Dev_GetClipStatus)                                                                           \
    X(Dev_GetTexture)                                                                              \
    X(Dev_SetTexture)                                                                              \
    X(Dev_GetTextureStageState)                                                                    \
    X(Dev_SetTextureStageState)                                                                    \
    X(Dev_GetSamplerState)                                                                         \
    X(Dev_SetSamplerState)                                                                         \
    X(Dev_SetPaletteEntries)                                                                       \
    X(Dev_GetPaletteEntries)                                                                       \
    X(Dev_SetCurrentTexturePalette)                                                                \
    X(Dev_GetCurrentTexturePalette)                                                                \
    X(Dev_DrawPrimitive)                                                                           \
    X(Dev_DrawIndexedPrimitive)                                                                    \
    X(Dev_DrawPrimitiveUP)                                                                         \
    X(Dev_DrawIndexedPrimitiveUP)                                                                  \
    X(Dev_SetFVF)                                                                                  \
    X(Dev_SetVertexShaderConstantF)                                                                \
    X(Dev_SetPixelShaderConstantF)                                                                 \
    X(Dev_SetStreamSource)                                                                         \
    X(Dev_SetIndices)                                                                              \
    X(Res_GetDevice)                                                                               \
    X(Res_SetPrivateData)                                                                          \
    X(Res_GetPrivateData)                                                                          \
    X(Res_FreePrivateData)                                                                         \
    X(Res_SetPriority)                                                                             \
    X(Res_GetPriority)                                                                             \
    X(Res_PreLoad)                                                                                 \
    X(Tex_SetLOD)                                                                                  \
    X(Tex_GetLOD)                                                                                  \
    X(Tex_GetLevelCount)                                                                           \
    X(Tex_GetLevelDesc)                                                                            \
    X(Tex_GetSurfaceLevel)                                                                         \
    X(Tex_LockRect)                                                                                \
    X(Tex_UnlockRect)                                                                              \
    X(Tex_AddDirtyRect)                                                                            \
    X(Tex_GetCubeMapSurface)                                                                       \
    X(Tex_LockRectCube)                                                                            \
    X(Tex_UnlockRectCube)                                                                          \
    X(Tex_AddDirtyRectCube)                                                                        \
    X(Surf_GetContainer)                                                                           \
    X(Surf_GetDesc)                                                                                \
    X(Surf_LockRect)                                                                               \
    X(Surf_UnlockRect)                                                                             \
    X(VB_Lock)                                                                                     \
    X(IB_Lock)                                                                                     \
    X(Buf_Unlock)                                                                                  \
    X(Buf_GetDesc)
#define D9_DECLARE(name) void name(X86 *c);
D9_METHODS(D9_DECLARE)
#undef D9_DECLARE
uint32_t d9_fvf_decl_object(ComObj *dev, uint32_t fvf);

namespace {

// ---------------------------------------------------------------------------
// Views and scratch memory
// ---------------------------------------------------------------------------
// The version 8 interface an object is seen through.
ComIface iface8_of(ComObj *o) {
    if (!o)
        return IF_NONE;
    switch (o->kind) {
    case K_D3D9:
        return IF_D3D8;
    case K_D3D9DEVICE:
        return IF_D3DDEVICE8;
    case K_D3D9TEXTURE:
        return o->caps ? IF_D3DCUBETEXTURE8 : IF_D3DTEXTURE8;
    case K_D3D9SURFACE:
        return IF_D3DSURFACE8;
    case K_D3D9VB:
        return IF_D3DVERTEXBUFFER8;
    case K_D3D9IB:
        return IF_D3DINDEXBUFFER8;
    default:
        return IF_NONE;
    }
}
// Rewrites the interface pointer at `out`, which a version 9 method just
// stored, as the same object's version 8 view.
void out_as8(uint32_t out) {
    if (!out || !gm_valid(out, 4))
        return;
    uint32_t v = rd32(out);
    ComObj *o = v ? com_this(v) : nullptr;
    ComIface want = iface8_of(o);
    if (o && want != IF_NONE)
        wr32(out, com_view(o, want));
}
// A zeroed guest block that lives until the end of the calling method.
struct Scratch {
    uint32_t at = 0;
    explicit Scratch(uint32_t bytes) {
        at = heap_alloc(bytes, true, 16);
    }
    ~Scratch() {
        if (at)
            heap_free(at);
    }
};
ComObj *this_device8(X86 *c) {
    ComObj *o = com_this_arg(c);
    return (o && o->kind == K_D3D9DEVICE) ? o : nullptr;
}

// Per-device state version 8 has and version 9 does not.
struct Shader8 {
    std::vector<uint8_t> decl;       // D3DVERTEXELEMENT9 records, empty for none
    std::vector<uint32_t> decl8;     // the game's own declaration tokens
    std::vector<uint32_t> function8; // the game's own function tokens
    D9ShaderBytes code;              // what the version 9 pipeline runs
    std::vector<std::pair<uint32_t, std::array<float, 4>>> constants; // CONSTMEM
    uint32_t decl_object = 0;
};
struct Device8 {
    uint32_t base_vertex = 0; // SetIndices' BaseVertexIndex
    uint32_t vs_handle = 0, ps_handle = 0;
};
std::map<uint32_t, Device8> &devices8() {
    static auto *m = new std::map<uint32_t, Device8>();
    return *m;
}
std::map<uint32_t, Shader8> &vertex_shaders() { // by handle
    static auto *m = new std::map<uint32_t, Shader8>();
    return *m;
}
std::map<uint32_t, Shader8> &pixel_shaders() {
    static auto *m = new std::map<uint32_t, Shader8>();
    return *m;
}
Device8 &dev8(ComObj *dev) {
    return devices8()[dev ? dev->id : 0];
}

} // namespace

// ---------------------------------------------------------------------------
// IDirect3D8
// ---------------------------------------------------------------------------
namespace {
// The display modes offered: the 4:3 sizes games of the time list, in the
// two formats they ask for (X8R8G8B8 22 and R5G6B5 23).
struct Mode8 {
    uint32_t w, h, format;
};
const Mode8 kModes[] = {{640, 480, 22}, {800, 600, 22}, {1024, 768, 22}, {1280, 960, 22},
                        {640, 480, 23}, {800, 600, 23}, {1024, 768, 23}, {1280, 960, 23}};

void put_mode(uint32_t p, const Mode8 &m) {
    wr32(p + 0, m.w);
    wr32(p + 4, m.h);
    wr32(p + 8, 60);
    wr32(p + 12, m.format);
}

// D3DPRESENT_PARAMETERS: version 9 inserts MultiSampleQuality at 20.
void present8_to_9(uint32_t p8, uint32_t p9) {
    for (uint32_t o = 0; o < 20; o += 4)
        wr32(p9 + o, rd32(p8 + o));
    wr32(p9 + 20, 0);
    for (uint32_t o = 20; o < 52; o += 4)
        wr32(p9 + o + 4, rd32(p8 + o));
}

// D3DCAPS8 is the first 212 bytes of D3DCAPS9, with the shader models of
// the version 8 runtime: vs_1_1 with 96 constants and ps_1_4.
void caps9_to_8(uint32_t c9, uint32_t c8) {
    memcpy(gm_ptr(c8), gm_ptr(c9), 212);
    // D3DCAPS2_CANRENDERWINDOWED: version 8 asks before it offers a window
    // (the SDK's D3DApp framework refuses to run windowed without it).
    wr32(c8 + 12, rd32(c8 + 12) | 0x00080000u);
    wr32(c8 + 196, 0xfffe0101u);
    wr32(c8 + 200, 96);
    wr32(c8 + 204, 0xffff0104u);
    float max_value = 8.0f;
    memcpy(gm_ptr(c8 + 208), &max_value, 4);
}
} // namespace

// (this, Adapter, Flags, pIdentifier): D3DADAPTER_IDENTIFIER8 has no
// DeviceName, so everything after the descriptions moves up 32 bytes.
static void D8_GetAdapterIdentifier(X86 *c) {
    uint32_t out = arg(c, 3);
    Scratch id9(1100);
    if (!out || !id9.at || !gm_fits(out, 1068)) {
        com_ret(c, D3DERR_INVALIDCALL8);
        return;
    }
    shim_forward(c, D9_GetAdapterIdentifier, {arg(c, 0), arg(c, 1), arg(c, 2), id9.at});
    memcpy(gm_ptr(out), gm_ptr(id9.at), 1024);
    memcpy(gm_ptr(out + 1024), gm_ptr(id9.at + 1056), 1068 - 1024);
    com_ret(c, D3D_OK8);
}
// (this, Adapter)
static void D8_GetAdapterModeCount(X86 *c) {
    set_eax(c, (uint32_t)std::size(kModes));
}
// (this, Adapter, Mode, pMode)
static void D8_EnumAdapterModes(X86 *c) {
    uint32_t mode = arg(c, 2), out = arg(c, 3);
    if (mode >= std::size(kModes) || !out || !gm_fits(out, 16)) {
        com_ret(c, D3DERR_INVALIDCALL8);
        return;
    }
    put_mode(out, kModes[mode]);
    com_ret(c, D3D_OK8);
}
// (this, Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType)
static void D8_CheckDeviceMultiSampleType(X86 *c) {
    shim_forward(c, D9_CheckDeviceMultiSampleType,
                 {arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), arg(c, 5), 0});
}
// (this, Adapter, DeviceType, pCaps)
static void D8_GetDeviceCaps(X86 *c) {
    uint32_t out = arg(c, 3);
    Scratch caps(304);
    if (!out || !caps.at || !gm_fits(out, 212)) {
        com_ret(c, D3DERR_INVALIDCALL8);
        return;
    }
    shim_forward(c, D9_GetDeviceCaps, {arg(c, 0), arg(c, 1), arg(c, 2), caps.at});
    caps9_to_8(caps.at, out);
    com_ret(c, D3D_OK8);
}
// (this, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters,
//  ppReturnedDeviceInterface)
static void D8_CreateDevice(X86 *c) {
    uint32_t pp8 = arg(c, 5), out = arg(c, 6);
    Scratch pp9(56);
    if (!pp8 || !out || !pp9.at || !gm_fits(pp8, 52)) {
        com_ret(c, D3DERR_INVALIDCALL8);
        return;
    }
    present8_to_9(pp8, pp9.at);
    shim_forward(c, D9_CreateDevice,
                 {arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), pp9.at, out});
    if (c->r[R_EAX] == 0) {
        out_as8(out);
        ComObj *dev = com_this(rd32(out));
        if (dev)
            devices8()[dev->id] = Device8();
        LOGW("d3d8: CreateDevice %ux%u format %u, %s, behaviour %08x -> %08x", rd32(pp8),
             rd32(pp8 + 4), rd32(pp8 + 8), rd32(pp8 + 28) ? "windowed" : "full screen", arg(c, 4),
             rd32(out));
    }
}

static const ComMethod g_d3d8[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"RegisterSoftwareDevice", 2, D9_RegisterSoftwareDevice},
    {"GetAdapterCount", 1, D9_GetAdapterCount},
    {"GetAdapterIdentifier", 4, D8_GetAdapterIdentifier},
    {"GetAdapterModeCount", 2, D8_GetAdapterModeCount},
    {"EnumAdapterModes", 4, D8_EnumAdapterModes},
    {"GetAdapterDisplayMode", 3, D9_GetAdapterDisplayMode},
    {"CheckDeviceType", 6, D9_CheckDeviceType},
    {"CheckDeviceFormat", 7, D9_CheckDeviceFormat},
    {"CheckDeviceMultiSampleType", 6, D8_CheckDeviceMultiSampleType},
    {"CheckDepthStencilMatch", 6, D9_CheckDepthStencilMatch},
    {"GetDeviceCaps", 4, D8_GetDeviceCaps},
    {"GetAdapterMonitor", 2, D9_GetAdapterMonitor},
    {"CreateDevice", 7, D8_CreateDevice},
};

// ---------------------------------------------------------------------------
// IDirect3DDevice8: the slots whose arguments changed
// ---------------------------------------------------------------------------
#define D8_STUB(name, hr)                                                                          \
    static void D8_##name(X86 *c) {                                                                \
        log_once("d3d8.dev." #name, "d3d8: IDirect3DDevice8::" #name " is not implemented");       \
        com_ret(c, hr);                                                                            \
    }
D8_STUB(ResourceManagerDiscardBytes, D3D_OK8)
D8_STUB(CreateAdditionalSwapChain, D3DERR_NOTAVAILABLE8)
D8_STUB(SetGammaRamp, D3D_OK8)
D8_STUB(CreateVolumeTexture, D3DERR_NOTAVAILABLE8)
D8_STUB(GetFrontBuffer, D3D_OK8)
D8_STUB(ProcessVertices, D3D_OK8)
D8_STUB(DrawRectPatch, D3D_OK8)
D8_STUB(DrawTriPatch, D3D_OK8)
D8_STUB(DeletePatch, D3D_OK8)
D8_STUB(GetInfo, 1u /* S_FALSE: no such information */)

// (this, ppD3D8)
static void D8_GetDirect3D(X86 *c) {
    shim_forward(c, Dev_GetDirect3D, {arg(c, 0), arg(c, 1)});
    out_as8(arg(c, 1));
}
// (this, pCaps)
static void D8_DevGetDeviceCaps(X86 *c) {
    uint32_t out = arg(c, 1);
    Scratch caps(304);
    if (!out || !caps.at || !gm_fits(out, 212)) {
        com_ret(c, D3DERR_INVALIDCALL8);
        return;
    }
    shim_forward(c, Dev_GetDeviceCaps, {arg(c, 0), caps.at});
    caps9_to_8(caps.at, out);
    com_ret(c, D3D_OK8);
}
// (this, pMode): the back buffer's size in X8R8G8B8.
static void D8_GetDisplayMode(X86 *c) {
    ComObj *dev = this_device8(c);
    uint32_t out = arg(c, 1);
    if (!dev || !out || !gm_fits(out, 16)) {
        com_ret(c, D3DERR_INVALIDCALL8);
        return;
    }
    put_mode(out, {dev->width ? dev->width : 640, dev->height ? dev->height : 480, 22});
    com_ret(c, D3D_OK8);
}
// (this, pParameters): D3DDEVICE_CREATION_PARAMETERS.
static void D8_GetCreationParameters(X86 *c) {
    ComObj *dev = this_device8(c);
    uint32_t out = arg(c, 1);
    if (!dev || !out || !gm_fits(out, 16)) {
        com_ret(c, D3DERR_INVALIDCALL8);
        return;
    }
    wr32(out + 0, 0);         // AdapterOrdinal
    wr32(out + 4, 1);         // D3DDEVTYPE_HAL
    wr32(out + 8, dev->hwnd); // hFocusWindow
    wr32(out + 12, 0x40);     // D3DCREATE_HARDWARE_VERTEXPROCESSING
    com_ret(c, D3D_OK8);
}
// (this, pPresentationParameters)
static void D8_Reset(X86 *c) {
    uint32_t pp8 = arg(c, 1);
    Scratch pp9(56);
    if (!pp8 || !pp9.at || !gm_fits(pp8, 52)) {
        com_ret(c, D3DERR_INVALIDCALL8);
        return;
    }
    present8_to_9(pp8, pp9.at);
    shim_forward(c, Dev_Reset, {arg(c, 0), pp9.at});
}
// (this, BackBuffer, Type, ppBackBuffer)
static void D8_GetBackBuffer(X86 *c) {
    shim_forward(c, Dev_GetBackBuffer, {arg(c, 0), 0, arg(c, 1), arg(c, 2), arg(c, 3)});
    out_as8(arg(c, 3));
}
// (this, pRasterStatus): never in vertical blank, at scan line 0.
static void D8_GetRasterStatus(X86 *c) {
    uint32_t out = arg(c, 1);
    if (out && gm_fits(out, 8)) {
        wr32(out, 0);
        wr32(out + 4, 0);
    }
    com_ret(c, out ? D3D_OK8 : D3DERR_INVALIDCALL8);
}
// (this, pRamp): the identity ramp, three tables of 256 WORDs.
static void D8_GetGammaRamp(X86 *c) {
    uint32_t out = arg(c, 1);
    if (out && gm_fits(out, 1536))
        for (uint32_t t = 0; t < 3; ++t)
            for (uint32_t i = 0; i < 256; ++i)
                wr16(out + 512 * t + 2 * i, (uint16_t)(i * 257));
    com_ret(c, D3D_OK8);
}

// The resource factories: version 9 adds a shared-handle pointer to each,
// and a quality level to the multisampled surfaces.
// (this, Width, Height, Levels, Usage, Format, Pool, ppTexture)
static void D8_CreateTexture(X86 *c) {
    shim_forward(c, Dev_CreateTexture,
                 {arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), arg(c, 5), arg(c, 6),
                  arg(c, 7), 0});
    out_as8(arg(c, 7));
}
// (this, EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture)
static void D8_CreateCubeTexture(X86 *c) {
    shim_forward(c, Dev_CreateCubeTexture,
                 {arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), arg(c, 5), arg(c, 6), 0});
    out_as8(arg(c, 6));
}
// (this, Length, Usage, FVF, Pool, ppVertexBuffer)
static void D8_CreateVertexBuffer(X86 *c) {
    shim_forward(c, Dev_CreateVertexBuffer,
                 {arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), arg(c, 5), 0});
    out_as8(arg(c, 5));
}
// (this, Length, Usage, Format, Pool, ppIndexBuffer)
static void D8_CreateIndexBuffer(X86 *c) {
    shim_forward(c, Dev_CreateIndexBuffer,
                 {arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), arg(c, 5), 0});
    out_as8(arg(c, 5));
}
// (this, Width, Height, Format, MultiSample, Lockable, ppSurface)
static void D8_CreateRenderTarget(X86 *c) {
    shim_forward(
        c, Dev_CreateRenderTarget,
        {arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), 0, arg(c, 5), arg(c, 6), 0});
    out_as8(arg(c, 6));
}
// (this, Width, Height, Format, MultiSample, ppSurface)
static void D8_CreateDepthStencilSurface(X86 *c) {
    shim_forward(c, Dev_CreateDepthStencilSurface,
                 {arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), 0, 0, arg(c, 5), 0});
    out_as8(arg(c, 5));
}
// (this, Width, Height, Format, ppSurface): a system-memory surface.
static void D8_CreateImageSurface(X86 *c) {
    shim_forward(
        c, Dev_CreateOffscreenPlainSurface,
        {arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), 2 /* D3DPOOL_SYSTEMMEM */, arg(c, 4), 0});
    out_as8(arg(c, 4));
}

namespace {
// Bytes per pixel of an uncompressed format, 0 for a block format.
uint32_t format_bytes8(uint32_t fmt) {
    switch (fmt) {
    case 27: // R3G3B2
    case 28: // A8
    case 41: // P8
    case 50: // L8
    case 52: // A4L4
        return 1;
    case 23: // R5G6B5
    case 24: // X1R5G5B5
    case 25: // A1R5G5B5
    case 26: // A4R4G4B4
    case 29: // A8R3G3B2
    case 30: // X4R4G4B4
    case 40: // A8P8
    case 51: // A8L8
    case 60: // V8U8
    case 61: // L6V5U5
    case 70: // D16_LOCKABLE
    case 80: // D16
        return 2;
    default:
        return fmt > 0xffff ? 0 : 4;
    }
}
// A locked surface: its pitch and the guest address of its bits.
struct Locked {
    uint32_t pitch = 0, bits = 0, width = 0, height = 0, bpp = 0;
};
bool lock_surface(X86 *c, uint32_t surface, Locked &l) {
    ComObj *s = com_this(surface);
    if (!s || s->kind != K_D3D9SURFACE)
        return false;
    Scratch out(8);
    if (!out.at)
        return false;
    shim_forward(c, Surf_LockRect, {surface, out.at, 0, 0});
    l.pitch = rd32(out.at);
    l.bits = rd32(out.at + 4);
    l.width = s->width;
    l.height = s->height;
    l.bpp = format_bytes8(s->rmask ? s->rmask : 22);
    return l.bits != 0;
}
void unlock_surface(X86 *c, uint32_t surface) {
    shim_forward(c, Surf_UnlockRect, {surface});
}
} // namespace

// (this, pSourceSurface, pSourceRectsArray, cRects, pDestinationSurface,
//  pDestPointsArray): byte copies between surfaces of one format, no scaling.
static void D8_CopyRects(X86 *c) {
    uint32_t src = arg(c, 1), rects = arg(c, 2), count = arg(c, 3), dst = arg(c, 4),
             points = arg(c, 5);
    Locked s, d;
    if (!lock_surface(c, src, s)) {
        com_ret(c, D3DERR_INVALIDCALL8);
        return;
    }
    if (!lock_surface(c, dst, d)) {
        unlock_surface(c, src);
        com_ret(c, D3DERR_INVALIDCALL8);
        return;
    }
    uint32_t bpp = s.bpp;
    if (!bpp || bpp != d.bpp) {
        log_once("d3d8.copyrects.format",
                 "d3d8: CopyRects between block-compressed or different formats is not copied");
    } else {
        uint32_t n = (rects && count) ? count : 1;
        for (uint32_t i = 0; i < n; ++i) {
            int32_t r[4] = {0, 0, (int32_t)s.width, (int32_t)s.height};
            if (rects && count && gm_fits(rects + 16 * i, 16))
                for (int k = 0; k < 4; ++k)
                    r[k] = (int32_t)rd32(rects + 16 * i + 4 * k);
            int32_t px = r[0], py = r[1];
            if (points && gm_fits(points + 8 * i, 8)) {
                px = (int32_t)rd32(points + 8 * i);
                py = (int32_t)rd32(points + 8 * i + 4);
            }
            r[0] = std::max(r[0], 0);
            r[1] = std::max(r[1], 0);
            r[2] = std::min(r[2], (int32_t)s.width);
            r[3] = std::min(r[3], (int32_t)s.height);
            for (int32_t y = r[1]; y < r[3]; ++y) {
                int32_t dy = py + (y - r[1]);
                if (dy < 0 || dy >= (int32_t)d.height)
                    continue;
                int32_t x0 = r[0], x1 = r[2], dx = px;
                if (dx < 0) {
                    x0 -= dx;
                    dx = 0;
                }
                x1 = std::min(x1, x0 + ((int32_t)d.width - dx));
                if (x1 <= x0)
                    continue;
                memmove(gm_ptr(d.bits + (uint32_t)dy * d.pitch + (uint32_t)dx * bpp),
                        gm_ptr(s.bits + (uint32_t)y * s.pitch + (uint32_t)x0 * bpp),
                        (uint32_t)(x1 - x0) * bpp);
            }
        }
    }
    unlock_surface(c, dst);
    unlock_surface(c, src);
    com_ret(c, D3D_OK8);
}

// (this, pSourceTexture, pDestinationTexture): every level the two share,
// copied whole.
static void D8_UpdateTexture(X86 *c) {
    ComObj *src = com_this(arg(c, 1)), *dst = com_this(arg(c, 2));
    if (!src || !dst || src->kind != K_D3D9TEXTURE || dst->kind != K_D3D9TEXTURE) {
        com_ret(c, D3DERR_INVALIDCALL8);
        return;
    }
    if (src->caps || dst->caps) {
        log_once("d3d8.updatetexture.cube", "d3d8: UpdateTexture on cube maps is not copied");
        com_ret(c, D3D_OK8);
        return;
    }
    size_t levels = std::min(src->surfaces.size(), dst->surfaces.size());
    for (size_t l = 0; l < levels; ++l) {
        ComObj *a = com_get(src->surfaces[l]), *b = com_get(dst->surfaces[l]);
        if (!a || !b)
            continue;
        uint32_t va = com_view(a, IF_D3DSURFACE8), vb = com_view(b, IF_D3DSURFACE8);
        Locked s, d;
        if (!lock_surface(c, va, s))
            continue;
        if (lock_surface(c, vb, d)) {
            uint32_t rows = std::min(s.height, d.height);
            if (s.bpp == 0) // block formats: rows of 4x4 blocks
                rows = (rows + 3) / 4;
            uint32_t bytes = std::min(s.pitch, d.pitch);
            for (uint32_t y = 0; y < rows; ++y)
                memmove(gm_ptr(d.bits + y * d.pitch), gm_ptr(s.bits + y * s.pitch), bytes);
            unlock_surface(c, vb);
        }
        unlock_surface(c, va);
    }
    com_ret(c, D3D_OK8);
}

// (this, pRenderTarget, pNewZStencil): either may be null; a null target
// keeps the current one, a null depth buffer means none.
static void D8_SetRenderTarget(X86 *c) {
    if (arg(c, 1)) {
        shim_forward(c, Dev_SetRenderTarget, {arg(c, 0), 0, arg(c, 1)});
        if (c->r[R_EAX] != 0)
            return;
    }
    shim_forward(c, Dev_SetDepthStencilSurface, {arg(c, 0), arg(c, 2)});
}
// (this, ppRenderTarget)
static void D8_GetRenderTarget(X86 *c) {
    ComObj *dev = this_device8(c);
    uint32_t out = arg(c, 1);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL8);
        return;
    }
    ComObj *s = com_get(dev->render_target);
    if (!s) { // not set yet: the back buffer
        shim_forward(c, Dev_GetBackBuffer, {arg(c, 0), 0, 0, 0, out});
        out_as8(out);
        return;
    }
    com_addref(s);
    com_out_ptr(out, com_view(s, IF_D3DSURFACE8));
    com_ret(c, D3D_OK8);
}
// (this, ppZStencilSurface)
static void D8_GetDepthStencilSurface(X86 *c) {
    ComObj *dev = this_device8(c);
    uint32_t out = arg(c, 1);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL8);
        return;
    }
    if (!com_get(dev->zbuffer_obj)) {
        com_out_ptr(out, 0);
        com_ret(c, D3DERR_NOTFOUND8);
        return;
    }
    shim_forward(c, Dev_GetDepthStencilSurface, {arg(c, 0), out});
    out_as8(out);
}

// (this, State, Value). D3DRS_ZBIAS (47), an integer 0-16, became the float
// D3DRS_DEPTHBIAS (195); both are kept so GetRenderState reads back what
// was set.
static void D8_SetRenderState(X86 *c) {
    uint32_t state = arg(c, 1), value = arg(c, 2);
    if (state == 47) {
        float bias = -(float)value * 0.000005f;
        uint32_t bits;
        memcpy(&bits, &bias, 4);
        shim_forward(c, Dev_SetRenderState, {arg(c, 0), 195, bits});
    }
    shim_forward(c, Dev_SetRenderState, {arg(c, 0), state, value});
}

namespace {
// D3DTEXTURESTAGESTATETYPE values version 9 moved to sampler states, as the
// D3DSAMPLERSTATETYPE they became; 0 for a texture stage state proper.
uint32_t sampler_state_of(uint32_t type) {
    switch (type) {
    case 13: // ADDRESSU
        return 1;
    case 14: // ADDRESSV
        return 2;
    case 25: // ADDRESSW
        return 3;
    case 15: // BORDERCOLOR
        return 4;
    case 16: // MAGFILTER
        return 5;
    case 17: // MINFILTER
        return 6;
    case 18: // MIPFILTER
        return 7;
    case 19: // MIPMAPLODBIAS
        return 8;
    case 20: // MAXMIPLEVEL
        return 9;
    case 21: // MAXANISOTROPY
        return 10;
    default:
        return 0;
    }
}
} // namespace

// (this, Stage, Type, Value)
static void D8_SetTextureStageState(X86 *c) {
    uint32_t stage = arg(c, 1), type = arg(c, 2), value = arg(c, 3);
    if (uint32_t s = sampler_state_of(type)) {
        // The cubic filters version 8 offered are linear here.
        if ((s == 5 || s == 6) && (value == 4 || value == 5))
            value = 2;
        shim_forward(c, Dev_SetSamplerState, {arg(c, 0), stage, s, value});
        return;
    }
    shim_forward(c, Dev_SetTextureStageState, {arg(c, 0), stage, type, value});
}
// (this, Stage, Type, pValue)
static void D8_GetTextureStageState(X86 *c) {
    uint32_t stage = arg(c, 1), type = arg(c, 2);
    if (uint32_t s = sampler_state_of(type)) {
        shim_forward(c, Dev_GetSamplerState, {arg(c, 0), stage, s, arg(c, 3)});
        return;
    }
    shim_forward(c, Dev_GetTextureStageState, {arg(c, 0), stage, type, arg(c, 3)});
}
// (this, Stage, ppTexture)
static void D8_GetTexture(X86 *c) {
    shim_forward(c, Dev_GetTexture, {arg(c, 0), arg(c, 1), arg(c, 2)});
    out_as8(arg(c, 2));
}
// (this, pNumPasses): everything renders in one pass.
static void D8_ValidateDevice(X86 *c) {
    uint32_t out = arg(c, 1);
    if (out && gm_valid(out, 4))
        wr32(out, 1);
    com_ret(c, D3D_OK8);
}

// (this, PrimitiveType, MinIndex, NumVertices, StartIndex, PrimitiveCount):
// the base vertex comes from SetIndices.
static void D8_DrawIndexedPrimitive(X86 *c) {
    ComObj *dev = this_device8(c);
    uint32_t base = dev ? dev8(dev).base_vertex : 0;
    shim_forward(c, Dev_DrawIndexedPrimitive,
                 {arg(c, 0), arg(c, 1), base, arg(c, 2), arg(c, 3), arg(c, 4), arg(c, 5)});
}
// (this, StreamNumber, pStreamData, Stride)
static void D8_SetStreamSource(X86 *c) {
    shim_forward(c, Dev_SetStreamSource, {arg(c, 0), arg(c, 1), arg(c, 2), 0, arg(c, 3)});
}
// (this, StreamNumber, ppStreamData, pStride)
static void D8_GetStreamSource(X86 *c) {
    ComObj *dev = this_device8(c);
    uint32_t stream = arg(c, 1), out = arg(c, 2), stride = arg(c, 3);
    if (!dev || stream >= 8 || !out) {
        com_ret(c, D3DERR_INVALIDCALL8);
        return;
    }
    const D9Pipeline::Stream &st = d9_pipeline(dev->id).stream[stream];
    ComObj *vb = com_get(st.vb);
    if (vb)
        com_addref(vb);
    com_out_ptr(out, vb ? com_view(vb, IF_D3DVERTEXBUFFER8) : 0);
    if (stride && gm_valid(stride, 4))
        wr32(stride, st.stride);
    com_ret(c, D3D_OK8);
}
// (this, pIndexData, BaseVertexIndex)
static void D8_SetIndices(X86 *c) {
    ComObj *dev = this_device8(c);
    if (dev)
        dev8(dev).base_vertex = arg(c, 2);
    shim_forward(c, Dev_SetIndices, {arg(c, 0), arg(c, 1)});
}
// (this, ppIndexData, pBaseVertexIndex)
static void D8_GetIndices(X86 *c) {
    ComObj *dev = this_device8(c);
    uint32_t out = arg(c, 1), base = arg(c, 2);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL8);
        return;
    }
    ComObj *ib = com_get(d9_pipeline(dev->id).index_buffer);
    if (ib)
        com_addref(ib);
    com_out_ptr(out, ib ? com_view(ib, IF_D3DINDEXBUFFER8) : 0);
    if (base && gm_valid(base, 4))
        wr32(base, dev8(dev).base_vertex);
    com_ret(c, D3D_OK8);
}

// ---------------------------------------------------------------------------
// Vertex and pixel shaders: integer handles
// ---------------------------------------------------------------------------
namespace {
// D3DVSDE_* registers of a version 8 declaration, as the version 9 usage
// and usage index they stand for. The same table names the shader's input
// registers, so the declaration and the shader's dcl instructions agree.
struct Usage {
    uint8_t usage, index;
};
Usage usage_of_register(uint32_t r) {
    switch (r) {
    case 0:
        return {0, 0}; // POSITION
    case 1:
        return {1, 0}; // BLENDWEIGHT
    case 2:
        return {2, 0}; // BLENDINDICES
    case 3:
        return {3, 0}; // NORMAL
    case 4:
        return {4, 0}; // PSIZE
    case 5:
        return {10, 0}; // DIFFUSE
    case 6:
        return {10, 1}; // SPECULAR
    case 15:
        return {0, 1}; // POSITION2
    case 16:
        return {3, 1}; // NORMAL2
    default:
        if (r >= 7 && r <= 14)
            return {5, (uint8_t)(r - 7)}; // TEXCOORD0-7
        return {5, (uint8_t)(r & 7)};
    }
}
// Bytes of a D3DVSDT_* type, which share D3DDECLTYPE's numbering.
uint32_t vsdt_bytes(uint32_t t) {
    static const uint8_t b[8] = {4, 8, 12, 16, 4, 4, 4, 8};
    return t < 8 ? b[t] : 4;
}

// Tokens until (and including) the terminator: D3DVSD_END for a
// declaration, the END instruction for a function (comments skipped).
std::vector<uint32_t> read_tokens(uint32_t p, bool function) {
    std::vector<uint32_t> t;
    for (uint32_t i = 0; i < 65536 && gm_fits(p + 4 * i, 4); ++i) {
        uint32_t v = rd32(p + 4 * i);
        t.push_back(v);
        if (function && i > 0 && (v & 0xffff) == 0xfffe && !(v & 0x80000000u)) {
            uint32_t n = (v >> 16) & 0x7fff;
            for (uint32_t k = 0; k < n && gm_fits(p + 4 * (i + 1 + k), 4); ++k)
                t.push_back(rd32(p + 4 * (i + 1 + k)));
            i += n;
            continue;
        }
        if (function ? v == 0x0000ffffu : v == 0xffffffffu)
            break;
    }
    return t;
}

// The declaration as D3DVERTEXELEMENT9 records, its constants, and the
// registers it fills.
void convert_declaration(const std::vector<uint32_t> &d, Shader8 &s, std::vector<uint32_t> &regs) {
    uint32_t stream = 0, offset = 0;
    for (size_t i = 0; i < d.size(); ++i) {
        uint32_t t = d[i];
        if (t == 0xffffffffu)
            break;
        switch (t >> 29) {
        case 1: // STREAM
            stream = t & 0xf;
            offset = 0;
            break;
        case 2: // STREAMDATA
            if (t & (1u << 28)) {
                offset += 4 * ((t >> 16) & 0xf); // SKIP
            } else {
                uint32_t type = (t >> 16) & 0xf, r = t & 0x1f;
                Usage u = usage_of_register(r);
                uint8_t e[8] = {(uint8_t)stream,
                                0,
                                (uint8_t)(offset & 0xff),
                                (uint8_t)(offset >> 8),
                                (uint8_t)type,
                                0,
                                u.usage,
                                u.index};
                s.decl.insert(s.decl.end(), e, e + 8);
                regs.push_back(r);
                offset += vsdt_bytes(type);
            }
            break;
        case 4: { // CONSTMEM
            uint32_t count = (t >> 25) & 0xf, at = t & 0x7f;
            for (uint32_t k = 0; k < count && i + 4 < d.size(); ++k) {
                std::array<float, 4> v;
                for (int j = 0; j < 4; ++j)
                    memcpy(&v[j], &d[i + 1 + j], 4);
                s.constants.push_back({at + k, v});
                i += 4;
            }
            break;
        }
        case 5: // EXT: extension data follows
            i += (t >> 24) & 0x1f;
            break;
        default: // NOP, TESSELLATOR
            break;
        }
    }
    if (!s.decl.empty()) {
        static const uint8_t end[8] = {0xff, 0, 0, 0, 17, 0, 0, 0};
        s.decl.insert(s.decl.end(), end, end + 8);
    }
}

// A version 8 vertex shader declares nothing; version 9 wants a dcl per
// input register. They go straight after the version token.
D9ShaderBytes vertex_code(const std::vector<uint32_t> &f, const std::vector<uint32_t> &regs) {
    D9ShaderBytes b;
    if (f.empty())
        return b;
    std::vector<uint32_t> t;
    t.push_back(f[0]);
    for (uint32_t r : regs) {
        Usage u = usage_of_register(r);
        t.push_back(31); // dcl
        t.push_back(0x80000000u | u.usage | (uint32_t)u.index << 16);
        t.push_back(0x80000000u | (r & 0x7ff) | 1u << 28 | 0xfu << 16); // vN.xyzw
    }
    t.insert(t.end(), f.begin() + 1, f.end());
    auto bytes = std::make_shared<std::vector<uint8_t>>(t.size() * 4);
    memcpy(bytes->data(), t.data(), bytes->size());
    b.bytes = bytes;
    return b;
}
D9ShaderBytes pixel_code(const std::vector<uint32_t> &f) {
    D9ShaderBytes b;
    auto bytes = std::make_shared<std::vector<uint8_t>>(f.size() * 4);
    memcpy(bytes->data(), f.data(), bytes->size());
    b.bytes = bytes;
    return b;
}
uint32_t g_next_shader = 1;

// Copies tokens to the guest the GetVertexShaderDeclaration way: a null
// buffer asks for the size.
void put_tokens(X86 *c, const std::vector<uint32_t> &t, uint32_t data, uint32_t size_ptr) {
    if (!size_ptr || !gm_valid(size_ptr, 4)) {
        com_ret(c, D3DERR_INVALIDCALL8);
        return;
    }
    uint32_t bytes = (uint32_t)t.size() * 4;
    if (!data) {
        wr32(size_ptr, bytes);
        com_ret(c, D3D_OK8);
        return;
    }
    if (rd32(size_ptr) < bytes || !gm_fits(data, bytes)) {
        wr32(size_ptr, bytes);
        com_ret(c, 0x88760868u); // D3DERR_MOREDATA
        return;
    }
    memcpy(gm_ptr(data), t.data(), bytes);
    wr32(size_ptr, bytes);
    com_ret(c, D3D_OK8);
}
} // namespace

// (this, pDeclaration, pFunction, pHandle, Usage)
static void D8_CreateVertexShader(X86 *c) {
    ComObj *dev = this_device8(c);
    uint32_t decl = arg(c, 1), function = arg(c, 2), out = arg(c, 3);
    if (!dev || !decl || !out || !gm_valid(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL8);
        return;
    }
    Shader8 s;
    s.decl8 = read_tokens(decl, false);
    std::vector<uint32_t> regs;
    convert_declaration(s.decl8, s, regs);
    if (function)
        s.function8 = read_tokens(function, true);
    s.code = vertex_code(s.function8, regs);
    if (!s.decl.empty()) {
        ComObj *d = com_new(K_D3D9DECL);
        if (d) {
            d->dev_d3d = dev->id;
            d->blob = s.decl;
            s.decl_object = d->id;
        }
    }
    uint32_t handle = 0x80000000u | (g_next_shader++ * 2 + 1);
    LOGV("d3d8: CreateVertexShader -> %08x: %u declaration tokens, %u function tokens", handle,
         (uint32_t)s.decl8.size(), (uint32_t)s.function8.size());
    vertex_shaders()[handle] = std::move(s);
    wr32(out, handle);
    com_ret(c, D3D_OK8);
}
// (this, Handle): an FVF code, or a handle from CreateVertexShader (bit 31).
static void D8_SetVertexShader(X86 *c) {
    ComObj *dev = this_device8(c);
    uint32_t handle = arg(c, 1);
    if (!dev) {
        com_ret(c, D3DERR_INVALIDCALL8);
        return;
    }
    D9Pipeline &pl = d9_pipeline(dev->id);
    dev8(dev).vs_handle = handle;
    if (!(handle & 0x80000000u)) {
        pl.vs = D9ShaderBytes();
        pl.vs_key = 0;
        shim_forward(c, Dev_SetFVF, {arg(c, 0), handle});
        return;
    }
    auto it = vertex_shaders().find(handle);
    if (it == vertex_shaders().end()) {
        com_ret(c, D3DERR_INVALIDCALL8);
        return;
    }
    const Shader8 &s = it->second;
    pl.vs = s.code;
    pl.vs_key = 0;
    pl.fvf = 0;
    pl.declaration = s.decl_object;
    dev->current_viewport = s.decl_object;
    for (const auto &k : s.constants)
        if (k.first < 256)
            memcpy(pl.vconst[k.first], k.second.data(), 16);
    com_ret(c, D3D_OK8);
}
static void D8_GetVertexShader(X86 *c) {
    ComObj *dev = this_device8(c);
    uint32_t out = arg(c, 1);
    if (!dev || !out || !gm_valid(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL8);
        return;
    }
    wr32(out, dev8(dev).vs_handle);
    com_ret(c, D3D_OK8);
}
static void D8_DeleteVertexShader(X86 *c) {
    vertex_shaders().erase(arg(c, 1));
    com_ret(c, D3D_OK8);
}
// (this, Register, pConstantData, ConstantCount)
static void D8_GetVertexShaderConstant(X86 *c) {
    ComObj *dev = this_device8(c);
    uint32_t r = arg(c, 1), out = arg(c, 2), n = arg(c, 3);
    if (!dev || !out || r + n > 256 || !gm_fits_n(out, n, 16)) {
        com_ret(c, D3DERR_INVALIDCALL8);
        return;
    }
    memcpy(gm_ptr(out), d9_pipeline(dev->id).vconst[r], 16u * n);
    com_ret(c, D3D_OK8);
}
// (this, Handle, pData, pSizeOfData)
static void D8_GetVertexShaderDeclaration(X86 *c) {
    auto it = vertex_shaders().find(arg(c, 1));
    if (it == vertex_shaders().end()) {
        com_ret(c, D3DERR_INVALIDCALL8);
        return;
    }
    put_tokens(c, it->second.decl8, arg(c, 2), arg(c, 3));
}
static void D8_GetVertexShaderFunction(X86 *c) {
    auto it = vertex_shaders().find(arg(c, 1));
    if (it == vertex_shaders().end()) {
        com_ret(c, D3DERR_INVALIDCALL8);
        return;
    }
    put_tokens(c, it->second.function8, arg(c, 2), arg(c, 3));
}
// (this, pFunction, pHandle): ps_1_x bytecode reads the same in version 9.
static void D8_CreatePixelShader(X86 *c) {
    uint32_t function = arg(c, 1), out = arg(c, 2);
    if (!function || !out || !gm_valid(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL8);
        return;
    }
    Shader8 s;
    s.function8 = read_tokens(function, true);
    s.code = pixel_code(s.function8);
    uint32_t handle = g_next_shader++;
    pixel_shaders()[handle] = std::move(s);
    wr32(out, handle);
    com_ret(c, D3D_OK8);
}
// (this, Handle): 0 returns to the fixed-function pipeline.
static void D8_SetPixelShader(X86 *c) {
    ComObj *dev = this_device8(c);
    uint32_t handle = arg(c, 1);
    if (!dev) {
        com_ret(c, D3DERR_INVALIDCALL8);
        return;
    }
    D9Pipeline &pl = d9_pipeline(dev->id);
    if (!handle) {
        pl.ps = D9ShaderBytes();
    } else {
        auto it = pixel_shaders().find(handle);
        if (it == pixel_shaders().end()) {
            com_ret(c, D3DERR_INVALIDCALL8);
            return;
        }
        pl.ps = it->second.code;
    }
    pl.ps_key = 0;
    dev8(dev).ps_handle = handle;
    com_ret(c, D3D_OK8);
}
static void D8_GetPixelShader(X86 *c) {
    ComObj *dev = this_device8(c);
    uint32_t out = arg(c, 1);
    if (!dev || !out || !gm_valid(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL8);
        return;
    }
    wr32(out, dev8(dev).ps_handle);
    com_ret(c, D3D_OK8);
}
static void D8_DeletePixelShader(X86 *c) {
    pixel_shaders().erase(arg(c, 1));
    com_ret(c, D3D_OK8);
}
static void D8_GetPixelShaderConstant(X86 *c) {
    ComObj *dev = this_device8(c);
    uint32_t r = arg(c, 1), out = arg(c, 2), n = arg(c, 3);
    if (!dev || !out || r + n > 32 || !gm_fits_n(out, n, 16)) {
        com_ret(c, D3DERR_INVALIDCALL8);
        return;
    }
    memcpy(gm_ptr(out), d9_pipeline(dev->id).pconst[r], 16u * n);
    com_ret(c, D3D_OK8);
}
static void D8_GetPixelShaderFunction(X86 *c) {
    auto it = pixel_shaders().find(arg(c, 1));
    if (it == pixel_shaders().end()) {
        com_ret(c, D3DERR_INVALIDCALL8);
        return;
    }
    put_tokens(c, it->second.function8, arg(c, 2), arg(c, 3));
}

// ---------------------------------------------------------------------------
// State blocks: integer tokens over snapshots of the pipeline record
// ---------------------------------------------------------------------------
namespace {
struct Block {
    uint32_t device = 0;
    D9Pipeline snap;
    Device8 snap8;
    bool rs[256] = {};
    bool tss[8][33] = {};
    bool samp[16][14] = {};
    bool xf[32] = {};
    bool material = false, lights = false, textures = false, viewport = false, vs = false,
         ps = false, streams = false, vconst = false, pconst = false;
};
std::map<uint32_t, Block> &blocks() {
    static auto *m = new std::map<uint32_t, Block>();
    return *m;
}
uint32_t g_next_block = 1;
// The recording in progress, by device: the state as it was at
// BeginStateBlock. Recorded changes are not applied to the device.
std::map<uint32_t, std::pair<D9Pipeline, Device8>> &recordings() {
    static auto *m = new std::map<uint32_t, std::pair<D9Pipeline, Device8>>();
    return *m;
}

void set_all(Block &b, bool vertex, bool pixel) {
    for (auto &x : b.rs)
        x = true;
    for (auto &s : b.tss)
        for (auto &x : s)
            x = true;
    for (auto &s : b.samp)
        for (auto &x : s)
            x = true;
    if (vertex) {
        b.lights = b.vs = b.vconst = b.streams = true;
    }
    if (pixel) {
        b.ps = b.pconst = true;
    }
    if (vertex && pixel) {
        for (auto &x : b.xf)
            x = true;
        b.material = b.textures = b.viewport = true;
    }
}

// Copies the parts `b` covers from one record to another.
void copy_parts(const Block &b, const D9Pipeline &from, const Device8 &from8, D9Pipeline &to,
                Device8 &to8) {
    for (int i = 0; i < 256; ++i)
        if (b.rs[i]) {
            to.rs[i] = from.rs[i];
            to.rs_set[i] = from.rs_set[i];
        }
    for (int s = 0; s < 8; ++s)
        for (int t = 0; t < 33; ++t)
            if (b.tss[s][t])
                to.tss[s][t] = from.tss[s][t];
    for (int s = 0; s < 16; ++s)
        for (int t = 0; t < 14; ++t)
            if (b.samp[s][t])
                to.sampler_state[s][t] = from.sampler_state[s][t];
    for (int i = 0; i < 32; ++i)
        if (b.xf[i])
            memcpy(to.transform[i], from.transform[i], sizeof to.transform[i]);
    if (b.material)
        memcpy(to.material, from.material, sizeof to.material);
    if (b.lights)
        to.lights = from.lights;
    if (b.textures)
        memcpy(to.sampler_tex, from.sampler_tex, sizeof to.sampler_tex);
    if (b.viewport) {
        memcpy(to.viewport, from.viewport, sizeof to.viewport);
        memcpy(to.viewport_z, from.viewport_z, sizeof to.viewport_z);
        to.viewport_set = from.viewport_set;
    }
    if (b.vs) {
        to.vs = from.vs;
        to.vs_key = from.vs_key;
        to.declaration = from.declaration;
        to.fvf = from.fvf;
        to8.vs_handle = from8.vs_handle;
    }
    if (b.ps) {
        to.ps = from.ps;
        to.ps_key = from.ps_key;
        to8.ps_handle = from8.ps_handle;
    }
    if (b.streams) {
        memcpy(to.stream, from.stream, sizeof to.stream);
        to.index_buffer = from.index_buffer;
        to8.base_vertex = from8.base_vertex;
    }
    if (b.vconst)
        memcpy(to.vconst, from.vconst, sizeof to.vconst);
    if (b.pconst)
        memcpy(to.pconst, from.pconst, sizeof to.pconst);
    to.states_changed();
}
// Marks what differs between two records.
void mark_changes(Block &b, const D9Pipeline &x, const D9Pipeline &y, const Device8 &x8,
                  const Device8 &y8) {
    for (int i = 0; i < 256; ++i)
        b.rs[i] = x.rs[i] != y.rs[i] || x.rs_set[i] != y.rs_set[i];
    for (int s = 0; s < 8; ++s)
        for (int t = 0; t < 33; ++t)
            b.tss[s][t] = x.tss[s][t] != y.tss[s][t];
    for (int s = 0; s < 16; ++s)
        for (int t = 0; t < 14; ++t)
            b.samp[s][t] = x.sampler_state[s][t] != y.sampler_state[s][t];
    for (int i = 0; i < 32; ++i)
        b.xf[i] = memcmp(x.transform[i], y.transform[i], sizeof x.transform[i]) != 0;
    b.material = memcmp(x.material, y.material, sizeof x.material) != 0;
    b.lights = x.lights.size() != y.lights.size();
    for (auto i = x.lights.begin(), j = y.lights.begin(); !b.lights && i != x.lights.end();
         ++i, ++j)
        b.lights = i->first != j->first || i->second.enabled != j->second.enabled ||
                   memcmp(i->second.raw, j->second.raw, sizeof i->second.raw) != 0;
    b.textures = memcmp(x.sampler_tex, y.sampler_tex, sizeof x.sampler_tex) != 0;
    b.viewport =
        memcmp(x.viewport, y.viewport, sizeof x.viewport) != 0 || x.viewport_set != y.viewport_set;
    b.vs =
        x.vs.bytes != y.vs.bytes || x.declaration != y.declaration || x8.vs_handle != y8.vs_handle;
    b.ps = x.ps.bytes != y.ps.bytes || x8.ps_handle != y8.ps_handle;
    b.streams = memcmp(x.stream, y.stream, sizeof x.stream) != 0 ||
                x.index_buffer != y.index_buffer || x8.base_vertex != y8.base_vertex;
    b.vconst = memcmp(x.vconst, y.vconst, sizeof x.vconst) != 0;
    b.pconst = memcmp(x.pconst, y.pconst, sizeof x.pconst) != 0;
}
} // namespace

static void D8_BeginStateBlock(X86 *c) {
    ComObj *dev = this_device8(c);
    if (!dev || recordings().count(dev->id)) {
        com_ret(c, D3DERR_INVALIDCALL8);
        return;
    }
    recordings()[dev->id] = {d9_pipeline(dev->id), dev8(dev)};
    com_ret(c, D3D_OK8);
}
// (this, pToken)
static void D8_EndStateBlock(X86 *c) {
    ComObj *dev = this_device8(c);
    uint32_t out = arg(c, 1);
    auto it = dev ? recordings().find(dev->id) : recordings().end();
    if (it == recordings().end() || !out || !gm_valid(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL8);
        return;
    }
    D9Pipeline &pl = d9_pipeline(dev->id);
    Block b;
    b.device = dev->id;
    mark_changes(b, it->second.first, pl, it->second.second, dev8(dev));
    b.snap = pl;
    b.snap8 = dev8(dev);
    // The device goes back to how it was before the recording.
    copy_parts(b, it->second.first, it->second.second, pl, dev8(dev));
    recordings().erase(it);
    uint32_t token = g_next_block++;
    blocks()[token] = std::move(b);
    wr32(out, token);
    com_ret(c, D3D_OK8);
}
// (this, Token)
static void D8_ApplyStateBlock(X86 *c) {
    ComObj *dev = this_device8(c);
    auto it = blocks().find(arg(c, 1));
    if (!dev || it == blocks().end()) {
        com_ret(c, D3DERR_INVALIDCALL8);
        return;
    }
    copy_parts(it->second, it->second.snap, it->second.snap8, d9_pipeline(dev->id), dev8(dev));
    com_ret(c, D3D_OK8);
}
static void D8_CaptureStateBlock(X86 *c) {
    ComObj *dev = this_device8(c);
    auto it = blocks().find(arg(c, 1));
    if (!dev || it == blocks().end()) {
        com_ret(c, D3DERR_INVALIDCALL8);
        return;
    }
    copy_parts(it->second, d9_pipeline(dev->id), dev8(dev), it->second.snap, it->second.snap8);
    com_ret(c, D3D_OK8);
}
static void D8_DeleteStateBlock(X86 *c) {
    blocks().erase(arg(c, 1));
    com_ret(c, D3D_OK8);
}
// (this, Type, pToken): D3DSBT_ALL 1, PIXELSTATE 2, VERTEXSTATE 3.
static void D8_CreateStateBlock(X86 *c) {
    ComObj *dev = this_device8(c);
    uint32_t type = arg(c, 1), out = arg(c, 2);
    if (!dev || type < 1 || type > 3 || !out || !gm_valid(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL8);
        return;
    }
    Block b;
    b.device = dev->id;
    set_all(b, type != 2, type != 3);
    b.snap = d9_pipeline(dev->id);
    b.snap8 = dev8(dev);
    uint32_t token = g_next_block++;
    blocks()[token] = std::move(b);
    wr32(out, token);
    com_ret(c, D3D_OK8);
}

// ---------------------------------------------------------------------------
// The resources
// ---------------------------------------------------------------------------
// (this, ppDevice)
static void R8_GetDevice(X86 *c) {
    shim_forward(c, Res_GetDevice, {arg(c, 0), arg(c, 1)});
    out_as8(arg(c, 1));
}
// D3DRESOURCETYPE: SURFACE 1, TEXTURE 3, CUBETEXTURE 5, VERTEXBUFFER 6,
// INDEXBUFFER 7.
static void R8_GetType(X86 *c) {
    ComObj *o = com_this_arg(c);
    uint32_t t = 0;
    if (o)
        switch (o->kind) {
        case K_D3D9SURFACE:
            t = 1;
            break;
        case K_D3D9TEXTURE:
            t = o->caps ? 5 : 3;
            break;
        case K_D3D9VB:
            t = 6;
            break;
        case K_D3D9IB:
            t = 7;
            break;
        default:
            break;
        }
    set_eax(c, t);
}
namespace {
// D3DSURFACE_DESC version 8: Size at 16 where version 9 has the
// multisample type, which moves to 20 over the quality level.
void fix_surface_desc(uint32_t d, uint32_t bytes) {
    wr32(d + 4, 1); // D3DRTYPE_SURFACE
    wr32(d + 16, bytes);
    wr32(d + 20, 0);
}
uint32_t level_bytes(ComObj *tex, uint32_t level) {
    if (!tex || level >= tex->surfaces.size())
        return 0;
    ComObj *s = com_get(tex->surfaces[level]);
    return s ? (uint32_t)s->blob.size() : 0;
}
} // namespace
// (this, pDesc)
static void S8_GetDesc(X86 *c) {
    ComObj *s = com_this_arg(c);
    shim_forward(c, Surf_GetDesc, {arg(c, 0), arg(c, 1)});
    if (c->r[R_EAX] == 0 && s)
        fix_surface_desc(arg(c, 1), (uint32_t)s->blob.size());
}
// (this, riid, ppContainer)
static void S8_GetContainer(X86 *c) {
    shim_forward(c, Surf_GetContainer, {arg(c, 0), arg(c, 1), arg(c, 2)});
    out_as8(arg(c, 2));
}
// (this, Level, pDesc)
static void T8_GetLevelDesc(X86 *c) {
    ComObj *t = com_this_arg(c);
    shim_forward(c, Tex_GetLevelDesc, {arg(c, 0), arg(c, 1), arg(c, 2)});
    if (c->r[R_EAX] == 0 && t)
        fix_surface_desc(arg(c, 2), level_bytes(t, t->caps ? arg(c, 1) * 6 : arg(c, 1)));
}
// (this, Level, ppSurfaceLevel)
static void T8_GetSurfaceLevel(X86 *c) {
    shim_forward(c, Tex_GetSurfaceLevel, {arg(c, 0), arg(c, 1), arg(c, 2)});
    out_as8(arg(c, 2));
}
// (this, FaceType, Level, ppCubeMapSurface)
static void T8_GetCubeMapSurface(X86 *c) {
    shim_forward(c, Tex_GetCubeMapSurface, {arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3)});
    out_as8(arg(c, 3));
}
// (this, pDesc): D3DVERTEXBUFFER_DESC adds the FVF at 20.
static void VB8_GetDesc(X86 *c) {
    shim_forward(c, Buf_GetDesc, {arg(c, 0), arg(c, 1)});
    if (c->r[R_EAX] == 0) {
        wr32(arg(c, 1) + 4, 6);
        wr32(arg(c, 1) + 20, 0);
    }
}
static void IB8_GetDesc(X86 *c) {
    ComObj *ib = com_this_arg(c);
    shim_forward(c, Buf_GetDesc, {arg(c, 0), arg(c, 1)});
    if (c->r[R_EAX] == 0 && ib) {
        wr32(arg(c, 1) + 0, ib->rmask == 4 ? 102 : 101); // D3DFMT_INDEX32 / INDEX16
        wr32(arg(c, 1) + 4, 7);
    }
}

#define RESOURCE8_HEAD                                                                             \
    {"QueryInterface", 3, com_QueryInterface}, {"AddRef", 1, com_AddRef},                          \
        {"Release", 1, com_Release}, {"GetDevice", 2, R8_GetDevice},                               \
        {"SetPrivateData", 5, Res_SetPrivateData}, {"GetPrivateData", 4, Res_GetPrivateData},      \
        {"FreePrivateData", 2, Res_FreePrivateData}, {"SetPriority", 2, Res_SetPriority},          \
        {"GetPriority", 1, Res_GetPriority}, {"PreLoad", 1, Res_PreLoad}, {                        \
        "GetType", 1, R8_GetType                                                                   \
    }

static const ComMethod g_vb8[] = {
    RESOURCE8_HEAD,
    {"Lock", 5, VB_Lock},
    {"Unlock", 1, Buf_Unlock},
    {"GetDesc", 2, VB8_GetDesc},
};
static const ComMethod g_ib8[] = {
    RESOURCE8_HEAD,
    {"Lock", 5, IB_Lock},
    {"Unlock", 1, Buf_Unlock},
    {"GetDesc", 2, IB8_GetDesc},
};
// A surface is not a resource in version 8: no priority, preload or type.
static const ComMethod g_surface8[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetDevice", 2, R8_GetDevice},
    {"SetPrivateData", 5, Res_SetPrivateData},
    {"GetPrivateData", 4, Res_GetPrivateData},
    {"FreePrivateData", 2, Res_FreePrivateData},
    {"GetContainer", 3, S8_GetContainer},
    {"GetDesc", 2, S8_GetDesc},
    {"LockRect", 4, Surf_LockRect},
    {"UnlockRect", 1, Surf_UnlockRect},
};
static const ComMethod g_texture8[] = {
    RESOURCE8_HEAD,
    {"SetLOD", 2, Tex_SetLOD},
    {"GetLOD", 1, Tex_GetLOD},
    {"GetLevelCount", 1, Tex_GetLevelCount},
    {"GetLevelDesc", 3, T8_GetLevelDesc},
    {"GetSurfaceLevel", 3, T8_GetSurfaceLevel},
    {"LockRect", 5, Tex_LockRect},
    {"UnlockRect", 2, Tex_UnlockRect},
    {"AddDirtyRect", 2, Tex_AddDirtyRect},
};
static const ComMethod g_cubetexture8[] = {
    RESOURCE8_HEAD,
    {"SetLOD", 2, Tex_SetLOD},
    {"GetLOD", 1, Tex_GetLOD},
    {"GetLevelCount", 1, Tex_GetLevelCount},
    {"GetLevelDesc", 3, T8_GetLevelDesc},
    {"GetCubeMapSurface", 4, T8_GetCubeMapSurface},
    {"LockRect", 6, Tex_LockRectCube},
    {"UnlockRect", 3, Tex_UnlockRectCube},
    {"AddDirtyRect", 3, Tex_AddDirtyRectCube},
};

// ---------------------------------------------------------------------------
// IDirect3DDevice8: 97 slots in the version 8 order
// ---------------------------------------------------------------------------
static const ComMethod g_device8[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"TestCooperativeLevel", 1, Dev_TestCooperativeLevel},
    {"GetAvailableTextureMem", 1, Dev_GetAvailableTextureMem},
    {"ResourceManagerDiscardBytes", 2, D8_ResourceManagerDiscardBytes},
    {"GetDirect3D", 2, D8_GetDirect3D},
    {"GetDeviceCaps", 2, D8_DevGetDeviceCaps},
    {"GetDisplayMode", 2, D8_GetDisplayMode},
    {"GetCreationParameters", 2, D8_GetCreationParameters},
    {"SetCursorProperties", 4, Dev_SetCursorProperties},
    {"SetCursorPosition", 4, Dev_SetCursorPosition},
    {"ShowCursor", 2, Dev_ShowCursor},
    {"CreateAdditionalSwapChain", 3, D8_CreateAdditionalSwapChain},
    {"Reset", 2, D8_Reset},
    {"Present", 5, Dev_Present},
    {"GetBackBuffer", 4, D8_GetBackBuffer},
    {"GetRasterStatus", 2, D8_GetRasterStatus},
    {"SetGammaRamp", 3, D8_SetGammaRamp},
    {"GetGammaRamp", 2, D8_GetGammaRamp},
    {"CreateTexture", 8, D8_CreateTexture},
    {"CreateVolumeTexture", 9, D8_CreateVolumeTexture},
    {"CreateCubeTexture", 7, D8_CreateCubeTexture},
    {"CreateVertexBuffer", 6, D8_CreateVertexBuffer},
    {"CreateIndexBuffer", 6, D8_CreateIndexBuffer},
    {"CreateRenderTarget", 7, D8_CreateRenderTarget},
    {"CreateDepthStencilSurface", 6, D8_CreateDepthStencilSurface},
    {"CreateImageSurface", 5, D8_CreateImageSurface},
    {"CopyRects", 6, D8_CopyRects},
    {"UpdateTexture", 3, D8_UpdateTexture},
    {"GetFrontBuffer", 2, D8_GetFrontBuffer},
    {"SetRenderTarget", 3, D8_SetRenderTarget},
    {"GetRenderTarget", 2, D8_GetRenderTarget},
    {"GetDepthStencilSurface", 2, D8_GetDepthStencilSurface},
    {"BeginScene", 1, Dev_BeginScene},
    {"EndScene", 1, Dev_EndScene},
    {"Clear", 7, Dev_Clear},
    {"SetTransform", 3, Dev_SetTransform},
    {"GetTransform", 3, Dev_GetTransform},
    {"MultiplyTransform", 3, Dev_MultiplyTransform},
    {"SetViewport", 2, Dev_SetViewport},
    {"GetViewport", 2, Dev_GetViewport},
    {"SetMaterial", 2, Dev_SetMaterial},
    {"GetMaterial", 2, Dev_GetMaterial},
    {"SetLight", 3, Dev_SetLight},
    {"GetLight", 3, Dev_GetLight},
    {"LightEnable", 3, Dev_LightEnable},
    {"GetLightEnable", 3, Dev_GetLightEnable},
    {"SetClipPlane", 3, Dev_SetClipPlane},
    {"GetClipPlane", 3, Dev_GetClipPlane},
    {"SetRenderState", 3, D8_SetRenderState},
    {"GetRenderState", 3, Dev_GetRenderState},
    {"BeginStateBlock", 1, D8_BeginStateBlock},
    {"EndStateBlock", 2, D8_EndStateBlock},
    {"ApplyStateBlock", 2, D8_ApplyStateBlock},
    {"CaptureStateBlock", 2, D8_CaptureStateBlock},
    {"DeleteStateBlock", 2, D8_DeleteStateBlock},
    {"CreateStateBlock", 3, D8_CreateStateBlock},
    {"SetClipStatus", 2, Dev_SetClipStatus},
    {"GetClipStatus", 2, Dev_GetClipStatus},
    {"GetTexture", 3, D8_GetTexture},
    {"SetTexture", 3, Dev_SetTexture},
    {"GetTextureStageState", 4, D8_GetTextureStageState},
    {"SetTextureStageState", 4, D8_SetTextureStageState},
    {"ValidateDevice", 2, D8_ValidateDevice},
    {"GetInfo", 4, D8_GetInfo},
    {"SetPaletteEntries", 3, Dev_SetPaletteEntries},
    {"GetPaletteEntries", 3, Dev_GetPaletteEntries},
    {"SetCurrentTexturePalette", 2, Dev_SetCurrentTexturePalette},
    {"GetCurrentTexturePalette", 2, Dev_GetCurrentTexturePalette},
    {"DrawPrimitive", 4, Dev_DrawPrimitive},
    {"DrawIndexedPrimitive", 6, D8_DrawIndexedPrimitive},
    {"DrawPrimitiveUP", 5, Dev_DrawPrimitiveUP},
    {"DrawIndexedPrimitiveUP", 9, Dev_DrawIndexedPrimitiveUP},
    {"ProcessVertices", 6, D8_ProcessVertices},
    {"CreateVertexShader", 5, D8_CreateVertexShader},
    {"SetVertexShader", 2, D8_SetVertexShader},
    {"GetVertexShader", 2, D8_GetVertexShader},
    {"DeleteVertexShader", 2, D8_DeleteVertexShader},
    {"SetVertexShaderConstant", 4, Dev_SetVertexShaderConstantF},
    {"GetVertexShaderConstant", 4, D8_GetVertexShaderConstant},
    {"GetVertexShaderDeclaration", 4, D8_GetVertexShaderDeclaration},
    {"GetVertexShaderFunction", 4, D8_GetVertexShaderFunction},
    {"SetStreamSource", 4, D8_SetStreamSource},
    {"GetStreamSource", 4, D8_GetStreamSource},
    {"SetIndices", 3, D8_SetIndices},
    {"GetIndices", 3, D8_GetIndices},
    {"CreatePixelShader", 3, D8_CreatePixelShader},
    {"SetPixelShader", 2, D8_SetPixelShader},
    {"GetPixelShader", 2, D8_GetPixelShader},
    {"DeletePixelShader", 2, D8_DeletePixelShader},
    {"SetPixelShaderConstant", 4, Dev_SetPixelShaderConstantF},
    {"GetPixelShaderConstant", 4, D8_GetPixelShaderConstant},
    {"GetPixelShaderFunction", 4, D8_GetPixelShaderFunction},
    {"DrawRectPatch", 4, D8_DrawRectPatch},
    {"DrawTriPatch", 4, D8_DrawTriPatch},
    {"DeletePatch", 2, D8_DeletePatch},
};
static_assert(std::size(g_device8) == 97, "IDirect3DDevice8 has 97 slots");
static_assert(std::size(g_d3d8) == 16, "IDirect3D8 has 16 slots");

// The DLL's export: Direct3DCreate8(SDKVersion).
static void d3d8_Direct3DCreate8(X86 *c) {
    ComObj *o = com_new(K_D3D9);
    uint32_t view = o ? com_view(o, IF_D3D8) : 0;
    if (!view) {
        if (o)
            com_release(o);
        set_eax(c, 0);
        return;
    }
    LOGW("d3d8: Direct3DCreate8(SDK %u) -> %08x", arg(c, 0), view);
    set_eax(c, view);
}

static const ImportShim g_d3d8_exports[] = {
    {"d3d8.dll", "Direct3DCreate8", 1, d3d8_Direct3DCreate8},
};

void d3d8_register() {
    static bool done = false;
    if (done)
        return;
    done = true;
    d3d9_register(); // the objects these are views of
    com_define(IF_D3D8, "d3d8.dll", "IDirect3D8", g_d3d8, std::size(g_d3d8));
    com_define(IF_D3DDEVICE8, "d3d8.dll", "IDirect3DDevice8", g_device8, std::size(g_device8));
    com_define(IF_D3DTEXTURE8, "d3d8.dll", "IDirect3DTexture8", g_texture8, std::size(g_texture8));
    com_define(IF_D3DCUBETEXTURE8, "d3d8.dll", "IDirect3DCubeTexture8", g_cubetexture8,
               std::size(g_cubetexture8));
    com_define(IF_D3DSURFACE8, "d3d8.dll", "IDirect3DSurface8", g_surface8, std::size(g_surface8));
    com_define(IF_D3DVERTEXBUFFER8, "d3d8.dll", "IDirect3DVertexBuffer8", g_vb8, std::size(g_vb8));
    com_define(IF_D3DINDEXBUFFER8, "d3d8.dll", "IDirect3DIndexBuffer8", g_ib8, std::size(g_ib8));
    com_bind(IF_D3D8, K_D3D9);
    com_bind(IF_D3DDEVICE8, K_D3D9DEVICE);
    com_bind(IF_D3DTEXTURE8, K_D3D9TEXTURE);
    com_bind(IF_D3DCUBETEXTURE8, K_D3D9TEXTURE);
    com_bind(IF_D3DSURFACE8, K_D3D9SURFACE);
    com_bind(IF_D3DVERTEXBUFFER8, K_D3D9VB);
    com_bind(IF_D3DINDEXBUFFER8, K_D3D9IB);
    com_register_iid(IF_D3D8, IID_IDirect3D8_);
    com_register_iid(IF_D3DDEVICE8, IID_IDirect3DDevice8_);
    com_register_iid(IF_D3DTEXTURE8, IID_IDirect3DTexture8_);
    com_register_iid(IF_D3DCUBETEXTURE8, IID_IDirect3DCubeTexture8_);
    com_register_iid(IF_D3DSURFACE8, IID_IDirect3DSurface8_);
    com_register_iid(IF_D3DVERTEXBUFFER8, IID_IDirect3DVertexBuffer8_);
    com_register_iid(IF_D3DINDEXBUFFER8, IID_IDirect3DIndexBuffer8_);
    imports_register(g_d3d8_exports, std::size(g_d3d8_exports));
}
