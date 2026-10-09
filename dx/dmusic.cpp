// dmusic.cpp - DirectMusic as an audio-path factory over DirectSound.
//
// A DirectX 8 game may create a DirectMusic performance only for its audio
// paths: InitAudio, CreateStandardAudioPath per voice, then GetObjectInPath
// for the path's DirectSound buffer, 3D buffer, the primary buffer's
// listener and the synth port. Those buffers are real dsound.cpp buffers.
//
// There is no synthesizer. Segments, MIDI messages and DLS downloads are
// accepted and recorded but make no sound; a game that plays only through
// the synth is silent here. Every call into an unimplemented method is
// logged once under its name.
#include "com.h"
#include "dx.h"
#include "../runtime/guest.h"
#include "../runtime/imports.h"
#include "../runtime/memory.h"
#include "../platform/os.h"

#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <iterator>
#include <string>

#define IID_BYTES(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7)                                         \
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

ComObj *dsound_make_buffer(X86 *c, ComObj *ds, uint32_t flags, uint32_t rate, uint16_t channels,
                           uint32_t bytes);

namespace {

const uint8_t CLSID_DirectMusic_[16] =
    IID_BYTES(0x636B9F10, 0x0C7D, 0x11D1, 0x95, 0xB2, 0x00, 0x20, 0xAF, 0xDC, 0x74, 0x21);
const uint8_t CLSID_DirectMusicPerformance_[16] =
    IID_BYTES(0xD2AC2881, 0xB39B, 0x11D1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xB1, 0xBD);
const uint8_t IID_IDirectMusic_[16] =
    IID_BYTES(0x6536115A, 0x7B2D, 0x11D2, 0xBA, 0x18, 0x00, 0x00, 0xF8, 0x75, 0xAC, 0x12);
const uint8_t IID_IDirectMusic2_[16] =
    IID_BYTES(0x6FC2CAE1, 0xBC78, 0x11D2, 0xAF, 0xA6, 0x00, 0xAA, 0x00, 0x24, 0xD8, 0xB6);
const uint8_t IID_IDirectMusic8_[16] =
    IID_BYTES(0x2D3629F7, 0x813D, 0x4939, 0x85, 0x08, 0xF0, 0x5C, 0x6B, 0x75, 0xFD, 0x97);
const uint8_t IID_IDirectMusicPerformance_[16] =
    IID_BYTES(0x07D43D03, 0x6523, 0x11D2, 0x87, 0x1D, 0x00, 0x60, 0x08, 0x93, 0xB1, 0xBD);
const uint8_t IID_IDirectMusicPerformance2_[16] =
    IID_BYTES(0x6FC2CAE0, 0xBC78, 0x11D2, 0xAF, 0xA6, 0x00, 0xAA, 0x00, 0x24, 0xD8, 0xB6);
const uint8_t IID_IDirectMusicPerformance8_[16] =
    IID_BYTES(0x679C4137, 0xC62E, 0x4147, 0xB2, 0xB4, 0x9D, 0x56, 0x9A, 0xCB, 0x25, 0x4C);
const uint8_t IID_IDirectMusicPort_[16] =
    IID_BYTES(0x08F2D8C9, 0x37C2, 0x11D2, 0xB9, 0xF9, 0x00, 0x00, 0xF8, 0x75, 0xAC, 0x12);
const uint8_t IID_IDirectMusicPortDownload_[16] =
    IID_BYTES(0xD2AC287A, 0xB39B, 0x11D1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xB1, 0xBD);
const uint8_t IID_IDirectMusicDownload_[16] =
    IID_BYTES(0xD2AC287B, 0xB39B, 0x11D1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xB1, 0xBD);
const uint8_t IID_IDirectMusicAudioPath_[16] =
    IID_BYTES(0xC87631F5, 0x23BE, 0x4986, 0x88, 0x36, 0x05, 0x83, 0x2F, 0xCC, 0x48, 0xF9);
const uint8_t IID_IDirectMusicGraph_[16] =
    IID_BYTES(0x2BEFC277, 0x5497, 0x11D2, 0xBC, 0xCB, 0x00, 0xA0, 0xC9, 0x22, 0xE6, 0xEB);
const uint8_t IID_IDirectMusicTool_[16] =
    IID_BYTES(0xD2AC28BA, 0xB39B, 0x11D1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xB1, 0xBD);
const uint8_t IID_IDirectMusicTool8_[16] =
    IID_BYTES(0x0E674303, 0x3B05, 0x11D3, 0x9B, 0xD1, 0xF9, 0xE7, 0xF0, 0xA0, 0x15, 0x36);

// DMUS_APATH_* standard audio path types.
enum : uint32_t {
    APATH_SHARED_STEREOPLUSREVERB = 1,
    APATH_DYNAMIC_3D = 6,
    APATH_DYNAMIC_MONO = 7,
    APATH_DYNAMIC_STEREO = 8,
};
// DMUS_PATH_* stages of GetObjectInPath.
enum : uint32_t {
    PATH_AUDIOPATH = 0x2000,
    PATH_AUDIOPATH_GRAPH = 0x2100,
    PATH_PERFORMANCE = 0x3000,
    PATH_PERFORMANCE_GRAPH = 0x3100,
    PATH_PORT = 0x4000,
    PATH_BUFFER = 0x6000,
    PATH_MIXIN_BUFFER = 0x7000,
    PATH_PRIMARY_BUFFER = 0x8000,
};

// The performance's clock: REFERENCE_TIME in 100 ns units from the first
// use, and music time at 120 beats a minute and 768 ticks a beat.
uint64_t g_clock_origin = 0;
uint64_t ref_now() {
    uint64_t ns = os_monotonic_ns();
    if (!g_clock_origin)
        g_clock_origin = ns - 100; // never zero
    return (ns - g_clock_origin) / 100;
}
const uint64_t MUSIC_TICKS_PER_SEC = 1536;
uint32_t ref_to_music(uint64_t rt) {
    return (uint32_t)(rt * MUSIC_TICKS_PER_SEC / 10000000ull);
}
uint64_t music_to_ref(uint32_t mt) {
    return (uint64_t)(int32_t)mt * 10000000ull / MUSIC_TICKS_PER_SEC;
}
uint64_t rd64(uint32_t a) {
    return (uint64_t)rd32(a) | ((uint64_t)rd32(a + 4) << 32);
}
void wr64(uint32_t a, uint64_t v) {
    wr32(a, (uint32_t)v);
    wr32(a + 4, (uint32_t)(v >> 32));
}
bool out_ok(uint32_t p, uint32_t n = 4) {
    return p && gm_valid(p, n);
}
ComObj *this_kind(X86 *c, ComKind k) {
    ComObj *o = com_this_arg(c);
    return o && o->kind == k ? o : nullptr;
}
// Hands `o` out through `want` with one new reference.
uint32_t hand_out(ComObj *o, ComIface want) {
    if (!o)
        return 0;
    uint32_t v = com_view(o, want);
    if (v)
        com_addref(o);
    return v;
}
void release_id(uint32_t &id) {
    if (ComObj *o = com_get(id))
        com_release(o);
    id = 0;
}

#define DM_OK(name)                                                                                \
    void name(X86 *c) {                                                                            \
        log_once("dmusic." #name, "dmusic: " #name " accepted; it has no effect here");            \
        set_eax(c, S_OK);                                                                          \
    }
#define DM_FAIL(name, hr)                                                                          \
    void name(X86 *c) {                                                                            \
        log_once("dmusic." #name, "dmusic: " #name " is not implemented; returning " #hr);         \
        set_eax(c, (uint32_t)(hr));                                                                \
    }

// ---------------------------------------------------------------------------
// The synth port and its download buffers
// ---------------------------------------------------------------------------
ComObj *new_port(uint32_t perf_id) {
    ComObj *p = com_new(K_DMPORT);
    p->dm_perf = perf_id;
    p->dl_next_id = 1;
    return p;
}

DM_OK(Port_PlayBuffer)
DM_OK(Port_SetReadNotificationHandle)
DM_FAIL(Port_Read, S_FALSE)
DM_FAIL(Port_DownloadInstrument, E_NOTIMPL)
DM_OK(Port_UnloadInstrument)
DM_FAIL(Port_GetLatencyClock, E_NOTIMPL)
DM_FAIL(Port_GetRunningStats, E_NOTIMPL)
DM_OK(Port_Compact)
DM_FAIL(Port_DeviceIoControl, E_NOTIMPL)
DM_OK(Port_SetNumChannelGroups)
DM_OK(Port_Activate)
DM_OK(Port_SetChannelPriority)
DM_OK(Port_SetDirectSound)

// DMUS_PORTCAPS: dwSize, dwFlags, guidPort, dwClass, dwType, dwMemorySize,
// dwMaxChannelGroups, dwMaxVoices, dwMaxAudioChannels, dwEffectFlags,
// wszDescription[128].
void fill_port_caps(uint32_t caps) {
    uint32_t size = rd32(caps);
    if (size < 0x38 || !gm_valid(caps, size))
        return;
    gm_zero(caps + 4, size - 4);
    wr32(caps + 0x04, 0x0f); // DLS, external clock, software synth, mono/stereo
    wr32(caps + 0x18, 1);    // DMUS_PC_OUTPUTCLASS
    wr32(caps + 0x1c, 2);    // DMUS_PORT_USER_MODE_SYNTH
    wr32(caps + 0x20, 0xffffffffu);
    wr32(caps + 0x24, 1000);
    wr32(caps + 0x28, 1000);
    wr32(caps + 0x2c, 2);
    if (size >= 0x38 + 2 * 21)
        gm_put_wstr(caps + 0x38, "Microsoft Synthesizer", 128);
}
void Port_GetCaps(X86 *c) {
    uint32_t caps = arg(c, 1);
    if (!out_ok(caps)) {
        set_eax(c, E_POINTER);
        return;
    }
    fill_port_caps(caps);
    set_eax(c, S_OK);
}
void Port_GetNumChannelGroups(X86 *c) {
    uint32_t out = arg(c, 1);
    if (out_ok(out))
        wr32(out, 1000);
    set_eax(c, out ? S_OK : E_POINTER);
}
void Port_GetChannelPriority(X86 *c) {
    uint32_t out = arg(c, 3);
    if (out_ok(out))
        wr32(out, 0x40000000u); // DAUD_STANDARD_VOICE_PRIORITY
    set_eax(c, out ? S_OK : E_POINTER);
}
// GetFormat(pWaveFormatEx, pdwWaveFormatExSize, pdwBufferSize): the synth's
// 16-bit stereo output at 22050 Hz.
void Port_GetFormat(X86 *c) {
    uint32_t wfx = arg(c, 1), size = arg(c, 2), bufsize = arg(c, 3);
    if (!out_ok(size)) {
        set_eax(c, E_POINTER);
        return;
    }
    if (wfx && rd32(size) >= 18 && gm_valid(wfx, 18)) {
        wr16(wfx + 0, 1);
        wr16(wfx + 2, 2);
        wr32(wfx + 4, 22050);
        wr32(wfx + 8, 22050 * 4);
        wr16(wfx + 12, 4);
        wr16(wfx + 14, 16);
        wr16(wfx + 16, 0);
    }
    wr32(size, 18);
    if (out_ok(bufsize))
        wr32(bufsize, 22050 * 4 / 4);
    set_eax(c, S_OK);
}

const ComMethod g_port[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"PlayBuffer", 2, Port_PlayBuffer},
    {"SetReadNotificationHandle", 2, Port_SetReadNotificationHandle},
    {"Read", 2, Port_Read},
    {"DownloadInstrument", 5, Port_DownloadInstrument},
    {"UnloadInstrument", 2, Port_UnloadInstrument},
    {"GetLatencyClock", 2, Port_GetLatencyClock},
    {"GetRunningStats", 2, Port_GetRunningStats},
    {"Compact", 1, Port_Compact},
    {"GetCaps", 2, Port_GetCaps},
    {"DeviceIoControl", 8, Port_DeviceIoControl},
    {"SetNumChannelGroups", 2, Port_SetNumChannelGroups},
    {"GetNumChannelGroups", 2, Port_GetNumChannelGroups},
    {"Activate", 2, Port_Activate},
    {"SetChannelPriority", 4, Port_SetChannelPriority},
    {"GetChannelPriority", 4, Port_GetChannelPriority},
    {"SetDirectSound", 3, Port_SetDirectSound},
    {"GetFormat", 4, Port_GetFormat},
};

// IDirectMusicPortDownload. A download buffer is guest memory the game fills
// with a DMUS_DOWNLOADINFO (dwDLType, dwDLId, ...) and its data; Download
// records it and GetBuffer finds it again by that id.
void PD_AllocateBuffer(X86 *c) {
    ComObj *port = this_kind(c, K_DMPORT);
    uint32_t size = arg(c, 1), out = arg(c, 2);
    if (!port || !out_ok(out) || !size || size > 64u * 1024 * 1024) {
        set_eax(c, out_ok(out) ? E_INVALIDARG : E_POINTER);
        return;
    }
    wr32(out, 0);
    ComObj *d = com_new(K_DMDOWNLOAD);
    d->dl_mem = heap_alloc(size, true);
    d->dl_size = size;
    d->dm_perf = port->id;
    if (!d->dl_mem) {
        com_release(d);
        set_eax(c, E_OUTOFMEMORY);
        return;
    }
    wr32(out, com_view(d, IF_DMDOWNLOAD));
    set_eax(c, S_OK);
}
void PD_GetBuffer(X86 *c) {
    ComObj *port = this_kind(c, K_DMPORT);
    uint32_t id = arg(c, 1), out = arg(c, 2);
    if (!port || !out_ok(out)) {
        set_eax(c, E_POINTER);
        return;
    }
    wr32(out, 0);
    for (uint32_t i = 1; i <= com_object_count(); ++i) {
        ComObj *d = com_get(i);
        if (d && d->kind == K_DMDOWNLOAD && d->dm_perf == port->id && d->dl_id == id && id) {
            wr32(out, hand_out(d, IF_DMDOWNLOAD));
            set_eax(c, S_OK);
            return;
        }
    }
    set_eax(c, E_FAIL);
}
void PD_GetDLId(X86 *c) {
    ComObj *port = this_kind(c, K_DMPORT);
    uint32_t out = arg(c, 1), count = arg(c, 2);
    if (!port || !out_ok(out)) {
        set_eax(c, E_POINTER);
        return;
    }
    wr32(out, port->dl_next_id);
    port->dl_next_id += count ? count : 1;
    set_eax(c, S_OK);
}
void PD_GetAppend(X86 *c) {
    uint32_t out = arg(c, 1);
    if (out_ok(out))
        wr32(out, 0);
    set_eax(c, out ? S_OK : E_POINTER);
}
void PD_Download(X86 *c) {
    ComObj *d = com_this(arg(c, 1));
    if (!d || d->kind != K_DMDOWNLOAD) {
        set_eax(c, E_POINTER);
        return;
    }
    d->dl_id = d->dl_size >= 8 ? rd32(d->dl_mem + 4) : 0;
    log_once("dmusic.download",
             "dmusic: DLS downloads are kept but no synthesizer plays them (silent)");
    set_eax(c, S_OK);
}
void PD_Unload(X86 *c) {
    ComObj *d = com_this(arg(c, 1));
    if (d && d->kind == K_DMDOWNLOAD)
        d->dl_id = 0;
    set_eax(c, S_OK);
}
const ComMethod g_portdownload[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetBuffer", 3, PD_GetBuffer},
    {"AllocateBuffer", 3, PD_AllocateBuffer},
    {"GetDLId", 3, PD_GetDLId},
    {"GetAppend", 2, PD_GetAppend},
    {"Download", 2, PD_Download},
    {"Unload", 2, PD_Unload},
};

// IDirectMusicDownload::GetBuffer(ppvBuffer, pdwSize).
void DL_GetBuffer(X86 *c) {
    ComObj *d = this_kind(c, K_DMDOWNLOAD);
    uint32_t pbuf = arg(c, 1), psize = arg(c, 2);
    if (!d || !out_ok(pbuf) || !out_ok(psize)) {
        set_eax(c, E_POINTER);
        return;
    }
    wr32(pbuf, d->dl_mem);
    wr32(psize, d->dl_size);
    set_eax(c, S_OK);
}
const ComMethod g_download[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetBuffer", 3, DL_GetBuffer},
};
void download_destroy(ComObj *d) {
    if (d->dl_mem)
        heap_free(d->dl_mem);
    d->dl_mem = 0;
}

// ---------------------------------------------------------------------------
// The output tool a stamped message ends at
// ---------------------------------------------------------------------------
DM_OK(Tool_Init)
void Tool_GetMsgDeliveryType(X86 *c) {
    uint32_t out = arg(c, 1);
    if (out_ok(out))
        wr32(out, 1); // DMUS_PMSGF_TOOL_IMMEDIATE
    set_eax(c, out ? S_OK : E_POINTER);
}
void Tool_GetMediaTypeArraySize(X86 *c) {
    uint32_t out = arg(c, 1);
    if (out_ok(out))
        wr32(out, 0);
    set_eax(c, out ? S_OK : E_POINTER);
}
DM_OK(Tool_GetMediaTypes)
DM_OK(Tool_ProcessPMsg)
DM_OK(Tool_Flush)
const ComMethod g_tool[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"Init", 2, Tool_Init},
    {"GetMsgDeliveryType", 2, Tool_GetMsgDeliveryType},
    {"GetMediaTypeArraySize", 2, Tool_GetMediaTypeArraySize},
    {"GetMediaTypes", 3, Tool_GetMediaTypes},
    {"ProcessPMsg", 3, Tool_ProcessPMsg},
    {"Flush", 5, Tool_Flush},
};

// ---------------------------------------------------------------------------
// IDirectMusic8
// ---------------------------------------------------------------------------
void DM_EnumPort(X86 *c) {
    uint32_t index = arg(c, 1), caps = arg(c, 2);
    if (!out_ok(caps)) {
        set_eax(c, E_POINTER);
        return;
    }
    if (index) {
        set_eax(c, S_FALSE);
        return;
    }
    fill_port_caps(caps);
    set_eax(c, S_OK);
}
DM_FAIL(DM_CreateMusicBuffer, E_NOTIMPL)
// CreatePort(rclsidPort, pPortParams, ppPort, pUnkOuter)
void DM_CreatePort(X86 *c) {
    ComObj *dm = this_kind(c, K_DMUSIC);
    uint32_t out = arg(c, 3);
    if (!dm || !out_ok(out)) {
        set_eax(c, E_POINTER);
        return;
    }
    wr32(out, com_view(new_port(0), IF_DMPORT));
    set_eax(c, S_OK);
}
void DM_EnumMasterClock(X86 *c) {
    set_eax(c, S_FALSE);
}
DM_FAIL(DM_GetMasterClock, E_NOTIMPL)
DM_OK(DM_SetMasterClock)
DM_OK(DM_Activate)
void DM_GetDefaultPort(X86 *c) {
    uint32_t out = arg(c, 1);
    if (out_ok(out, 16))
        gm_zero(out, 16);
    set_eax(c, out ? S_OK : E_POINTER);
}
DM_OK(DM_SetDirectSound)
DM_OK(DM_SetExternalMasterClock)
const ComMethod g_dmusic[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"EnumPort", 3, DM_EnumPort},
    {"CreateMusicBuffer", 4, DM_CreateMusicBuffer},
    {"CreatePort", 5, DM_CreatePort},
    {"EnumMasterClock", 3, DM_EnumMasterClock},
    {"GetMasterClock", 3, DM_GetMasterClock},
    {"SetMasterClock", 2, DM_SetMasterClock},
    {"Activate", 2, DM_Activate},
    {"GetDefaultPort", 2, DM_GetDefaultPort},
    {"SetDirectSound", 3, DM_SetDirectSound},
    {"SetExternalMasterClock", 2, DM_SetExternalMasterClock},
};

// ---------------------------------------------------------------------------
// Audio paths. A path owns one DirectSound buffer, made on first use: mono
// with 3D control for DYNAMIC_3D, mono for DYNAMIC_MONO, stereo otherwise,
// one second long at the performance's sample rate.
// ---------------------------------------------------------------------------
ComObj *path_buffer(X86 *c, ComObj *path) {
    if (ComObj *b = com_get(path->ap_buffer))
        return b;
    ComObj *perf = com_get(path->dm_perf);
    ComObj *ds = perf ? com_get(perf->dm_dsound) : nullptr;
    if (!ds)
        return nullptr;
    bool is3d = path->ap_type == APATH_DYNAMIC_3D;
    uint16_t channels = (is3d || path->ap_type == APATH_DYNAMIC_MONO) ? 1 : 2;
    uint32_t flags = DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLFX |
                     DSBCAPS_GETCURRENTPOSITION2 | (is3d ? DSBCAPS_CTRL3D : DSBCAPS_CTRLPAN);
    uint32_t rate = perf->dm_sample_rate ? perf->dm_sample_rate : 22050;
    ComObj *b = dsound_make_buffer(c, ds, flags, rate, channels, rate * 2u * channels);
    if (b)
        path->ap_buffer = b->id;
    return b;
}

ComObj *new_path(ComObj *perf, uint32_t type, uint32_t pchannels) {
    ComObj *p = com_new(K_DMAUDIOPATH);
    p->dm_perf = perf->id;
    p->ap_type = type;
    p->ap_pchannels = pchannels;
    p->ap_pchannel_base = perf->dm_next_pchannel;
    perf->dm_next_pchannel += (pchannels + 15) / 16 * 16;
    return p;
}

// GetObjectInPath(dwPChannel, dwStage, dwBuffer, guidObject, dwIndex,
// iidInterface, ppObject)
void AP_GetObjectInPath(X86 *c) {
    ComObj *path = this_kind(c, K_DMAUDIOPATH);
    uint32_t stage = arg(c, 2), riid = arg(c, 6), out = arg(c, 7);
    if (!path || !out_ok(out) || !out_ok(riid, 16)) {
        set_eax(c, E_POINTER);
        return;
    }
    wr32(out, 0);
    ComIface want = com_iface_for_iid(riid);
    ComObj *perf = com_get(path->dm_perf);
    ComObj *target = nullptr;
    switch (stage) {
    case PATH_BUFFER:
    case PATH_MIXIN_BUFFER:
        target = path_buffer(c, path);
        break;
    case PATH_PRIMARY_BUFFER:
        target = perf ? com_get(perf->dm_dsound) : nullptr; // the listener is on the device
        break;
    case PATH_PORT:
        target = perf ? com_get(perf->dm_port) : nullptr;
        break;
    case PATH_AUDIOPATH:
    case PATH_AUDIOPATH_GRAPH:
        target = path;
        break;
    case PATH_PERFORMANCE:
    case PATH_PERFORMANCE_GRAPH:
        target = perf;
        break;
    default:
        break;
    }
    if (!target || want == IF_NONE || !com_iface_binds(want, target->kind)) {
        char key[64];
        snprintf(key, sizeof key, "dmusic.path.%x", stage);
        log_once(key, "dmusic: GetObjectInPath stage %04x has no %s here", stage,
                 want == IF_NONE ? "such interface" : com_iface_name(want));
        set_eax(c, E_FAIL);
        return;
    }
    uint32_t v = hand_out(target, want);
    wr32(out, v);
    set_eax(c, v ? S_OK : E_OUTOFMEMORY);
}
void AP_Activate(X86 *c) {
    ComObj *path = this_kind(c, K_DMAUDIOPATH);
    if (path)
        path->ap_active = arg(c, 1) != 0;
    set_eax(c, path ? S_OK : E_POINTER);
}
// SetVolume(lVolume, lDuration): the buffer's volume, applied at once.
void AP_SetVolume(X86 *c) {
    ComObj *path = this_kind(c, K_DMAUDIOPATH);
    if (!path) {
        set_eax(c, E_POINTER);
        return;
    }
    path->ap_volume = (int32_t)arg(c, 1);
    if (ComObj *b = com_get(path->ap_buffer))
        b->volume = path->ap_volume;
    set_eax(c, S_OK);
}
void AP_ConvertPChannel(X86 *c) {
    ComObj *path = this_kind(c, K_DMAUDIOPATH);
    uint32_t out = arg(c, 2);
    if (!path || !out_ok(out)) {
        set_eax(c, E_POINTER);
        return;
    }
    wr32(out, path->ap_pchannel_base + arg(c, 1));
    set_eax(c, S_OK);
}
const ComMethod g_audiopath[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetObjectInPath", 8, AP_GetObjectInPath},
    {"Activate", 2, AP_Activate},
    {"SetVolume", 3, AP_SetVolume},
    {"ConvertPChannel", 3, AP_ConvertPChannel},
};
void path_destroy(ComObj *p) {
    release_id(p->ap_buffer);
}

// IDirectMusicGraph, on a path or on the performance. StampPMsg routes a
// message to the output tool: DMUS_PMSG has dwPChannel at +0x18, pTool at
// +0x20 and pGraph at +0x24. A path's PChannels are mapped to its block.
void Graph_StampPMsg(X86 *c) {
    ComObj *g = com_this_arg(c);
    uint32_t msg = arg(c, 1);
    if (!g || !out_ok(msg, 0x28)) {
        set_eax(c, E_POINTER);
        return;
    }
    ComObj *perf = g->kind == K_DMPERF ? g : com_get(g->dm_perf);
    if (g->kind == K_DMAUDIOPATH)
        wr32(msg + 0x18, g->ap_pchannel_base + rd32(msg + 0x18));
    ComObj *tool = perf ? com_get(perf->dm_tool) : nullptr;
    wr32(msg + 0x20, tool ? hand_out(tool, IF_DMTOOL) : 0);
    wr32(msg + 0x24, hand_out(g, IF_DMGRAPH));
    set_eax(c, S_OK);
}
// The game's own tools are guest objects; they are accepted but no message
// ever reaches them, since nothing here plays one.
DM_OK(Graph_InsertTool)
DM_FAIL(Graph_GetTool, E_FAIL)
DM_OK(Graph_RemoveTool)
const ComMethod g_graph[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"StampPMsg", 2, Graph_StampPMsg},
    {"InsertTool", 5, Graph_InsertTool},
    {"GetTool", 3, Graph_GetTool},
    {"RemoveTool", 2, Graph_RemoveTool},
};

// ---------------------------------------------------------------------------
// IDirectMusicPerformance8
// ---------------------------------------------------------------------------
// Connects the performance to a DirectSound device: the one *ppDS names, or a
// new one written back through ppDS. Makes the port and the output tool.
bool perf_connect(ComObj *perf, uint32_t ds_ptr_addr, uint32_t ds_ptr) {
    ComObj *ds = ds_ptr ? com_this(ds_ptr) : nullptr;
    if (ds && ds->kind != K_DSOUND)
        ds = nullptr;
    if (ds) {
        com_addref(ds);
    } else {
        ds = com_new(K_DSOUND);
        if (ds_ptr_addr)
            wr32(ds_ptr_addr, hand_out(ds, IF_DSOUND8));
    }
    release_id(perf->dm_dsound);
    perf->dm_dsound = ds->id;
    if (!perf->dm_port)
        perf->dm_port = new_port(perf->id)->id;
    if (!perf->dm_tool)
        perf->dm_tool = com_new(K_DMTOOL)->id;
    return true;
}
void write_dmusic(uint32_t pp) {
    if (out_ok(pp) && !rd32(pp))
        wr32(pp, com_view(com_new(K_DMUSIC), IF_DMUSIC8));
}

// Init(ppDirectMusic, pDirectSound, hWnd)
void Perf_Init(X86 *c) {
    ComObj *perf = this_kind(c, K_DMPERF);
    if (!perf) {
        set_eax(c, E_POINTER);
        return;
    }
    write_dmusic(arg(c, 1));
    perf_connect(perf, 0, arg(c, 2));
    set_eax(c, S_OK);
}
// InitAudio(ppDirectMusic, ppDirectSound, hWnd, dwDefaultPathType,
// dwPChannelCount, dwFlags, pParams). DMUS_AUDIOPARAMS: dwSize, fInitNow,
// dwValidData, dwFeatures, dwVoices, dwSampleRate, clsidDefaultSynth.
void Perf_InitAudio(X86 *c) {
    ComObj *perf = this_kind(c, K_DMPERF);
    uint32_t ppdm = arg(c, 1), ppds = arg(c, 2), type = arg(c, 4), pchannels = arg(c, 5);
    uint32_t params = arg(c, 7);
    if (!perf) {
        set_eax(c, E_POINTER);
        return;
    }
    if (params && out_ok(params, 0x18) && (rd32(params + 8) & 4) && rd32(params + 0x14))
        perf->dm_sample_rate = rd32(params + 0x14);
    write_dmusic(ppdm);
    perf_connect(perf, ppds, out_ok(ppds) ? rd32(ppds) : 0);
    if (type) {
        release_id(perf->dm_default_path);
        ComObj *path = new_path(perf, type, pchannels);
        path->ap_active = true;
        perf->dm_default_path = path->id;
    }
    LOGV("dmusic: InitAudio path type %u, %u PChannels, %u Hz", type, pchannels,
         perf->dm_sample_rate);
    set_eax(c, S_OK);
}
void Perf_PlaySegment(X86 *c) {
    uint32_t state = arg(c, 5);
    if (out_ok(state))
        wr32(state, 0);
    log_once("dmusic.play", "dmusic: segments are accepted but no synthesizer plays them");
    set_eax(c, S_OK);
}
void Perf_PlaySegmentEx(X86 *c) {
    uint32_t state = arg(c, 7);
    if (out_ok(state))
        wr32(state, 0);
    log_once("dmusic.play", "dmusic: segments are accepted but no synthesizer plays them");
    set_eax(c, S_OK);
}
DM_OK(Perf_Stop)
DM_OK(Perf_StopEx)
DM_FAIL(Perf_GetSegmentState, E_FAIL)
DM_OK(Perf_SetPrepareTime)
void Perf_GetDword(X86 *c, uint32_t v) {
    uint32_t out = arg(c, 1);
    if (out_ok(out))
        wr32(out, v);
    set_eax(c, out ? S_OK : E_POINTER);
}
void Perf_GetPrepareTime(X86 *c) {
    Perf_GetDword(c, 1000);
}
DM_OK(Perf_SetBumperLength)
void Perf_GetBumperLength(X86 *c) {
    Perf_GetDword(c, 50);
}
// SendPMsg takes ownership of the message: it is freed, unplayed.
void Perf_SendPMsg(X86 *c) {
    uint32_t msg = arg(c, 1);
    log_once("dmusic.send", "dmusic: performance messages are accepted but not played");
    if (msg)
        heap_free(msg);
    set_eax(c, msg ? S_OK : E_POINTER);
}
// MusicToReferenceTime(mtTime, prtTime)
void Perf_MusicToReferenceTime(X86 *c) {
    uint32_t out = arg(c, 2);
    if (out_ok(out, 8))
        wr64(out, music_to_ref(arg(c, 1)));
    set_eax(c, out ? S_OK : E_POINTER);
}
// ReferenceToMusicTime(rtTime (8 bytes), pmtTime)
void Perf_ReferenceToMusicTime(X86 *c) {
    uint32_t out = arg(c, 3);
    uint64_t rt = (uint64_t)arg(c, 1) | ((uint64_t)arg(c, 2) << 32);
    if (out_ok(out))
        wr32(out, ref_to_music(rt));
    set_eax(c, out ? S_OK : E_POINTER);
}
void Perf_IsPlaying(X86 *c) {
    set_eax(c, S_FALSE);
}
// GetTime(prtNow, pmtNow)
void Perf_GetTime(X86 *c) {
    uint32_t prt = arg(c, 1), pmt = arg(c, 2);
    uint64_t now = ref_now();
    if (out_ok(prt, 8))
        wr64(prt, now);
    if (out_ok(pmt))
        wr32(pmt, ref_to_music(now));
    set_eax(c, S_OK);
}
// AllocPMsg(cb, ppPMSG): zeroed guest memory with dwSize set.
void Perf_AllocPMsg(X86 *c) {
    uint32_t cb = arg(c, 1), out = arg(c, 2);
    if (!out_ok(out) || cb < 0x38 || cb > 65536) {
        set_eax(c, out_ok(out) ? E_INVALIDARG : E_POINTER);
        return;
    }
    uint32_t m = heap_alloc(cb, true);
    if (m)
        wr32(m, cb);
    wr32(out, m);
    set_eax(c, m ? S_OK : E_OUTOFMEMORY);
}
void Perf_FreePMsg(X86 *c) {
    uint32_t msg = arg(c, 1);
    if (msg)
        heap_free(msg);
    set_eax(c, msg ? S_OK : E_POINTER);
}
// ClonePMsg(pSourcePMSG, ppCopyPMSG)
void Perf_ClonePMsg(X86 *c) {
    uint32_t src = arg(c, 1), out = arg(c, 2);
    if (!out_ok(src) || !out_ok(out)) {
        set_eax(c, E_POINTER);
        return;
    }
    uint32_t cb = rd32(src);
    uint32_t m = (cb >= 0x38 && cb <= 65536 && gm_valid(src, cb)) ? heap_alloc(cb) : 0;
    if (m)
        gm_copy(m, src, cb);
    wr32(out, m);
    set_eax(c, m ? S_OK : E_OUTOFMEMORY);
}
void Perf_GetGraph(X86 *c) {
    ComObj *perf = this_kind(c, K_DMPERF);
    uint32_t out = arg(c, 1);
    if (!perf || !out_ok(out)) {
        set_eax(c, E_POINTER);
        return;
    }
    wr32(out, hand_out(perf, IF_DMGRAPH));
    set_eax(c, S_OK);
}
DM_OK(Perf_SetGraph)
DM_OK(Perf_SetNotificationHandle)
void Perf_GetNotificationPMsg(X86 *c) {
    uint32_t out = arg(c, 1);
    if (out_ok(out))
        wr32(out, 0);
    set_eax(c, S_FALSE);
}
DM_OK(Perf_AddNotificationType)
DM_OK(Perf_RemoveNotificationType)
DM_OK(Perf_AddPort)
DM_OK(Perf_RemovePort)
DM_OK(Perf_AssignPChannelBlock)
DM_OK(Perf_AssignPChannel)
// PChannelInfo(dwPChannel, ppPort, pdwGroup, pdwMChannel)
void Perf_PChannelInfo(X86 *c) {
    ComObj *perf = this_kind(c, K_DMPERF);
    uint32_t pch = arg(c, 1), pport = arg(c, 2), pgroup = arg(c, 3), pmch = arg(c, 4);
    if (!perf) {
        set_eax(c, E_POINTER);
        return;
    }
    if (out_ok(pport))
        wr32(pport, hand_out(com_get(perf->dm_port), IF_DMPORT));
    if (out_ok(pgroup))
        wr32(pgroup, 1 + pch / 16);
    if (out_ok(pmch))
        wr32(pmch, pch % 16);
    set_eax(c, S_OK);
}
DM_FAIL(Perf_DownloadInstrument, E_NOTIMPL)
DM_OK(Perf_Invalidate)
DM_FAIL(Perf_GetParam, E_FAIL)
DM_FAIL(Perf_GetParamEx, E_FAIL)
DM_OK(Perf_SetParam)
std::string guid_key(uint32_t g) {
    return std::string((const char *)gm_ptr(g), 16);
}
// GetGlobalParam(rguidType, pParam, dwSize): what SetGlobalParam stored, else
// zeroes (master volume 0 dB, auto-download off).
void Perf_GetGlobalParam(X86 *c) {
    ComObj *perf = this_kind(c, K_DMPERF);
    uint32_t g = arg(c, 1), p = arg(c, 2), size = arg(c, 3);
    if (!perf || !out_ok(g, 16) || !p || !size || !gm_valid(p, size)) {
        set_eax(c, E_POINTER);
        return;
    }
    gm_zero(p, size);
    auto it = perf->dm_globals.find(guid_key(g));
    if (it != perf->dm_globals.end())
        memcpy(gm_ptr(p), it->second.data(), std::min<size_t>(size, it->second.size()));
    set_eax(c, S_OK);
}
void Perf_SetGlobalParam(X86 *c) {
    ComObj *perf = this_kind(c, K_DMPERF);
    uint32_t g = arg(c, 1), p = arg(c, 2), size = arg(c, 3);
    if (!perf || !out_ok(g, 16) || !p || !size || size > 4096 || !gm_valid(p, size)) {
        set_eax(c, E_POINTER);
        return;
    }
    perf->dm_globals[guid_key(g)].assign(gm_ptr(p), gm_ptr(p) + size);
    set_eax(c, S_OK);
}
void Perf_GetLatencyTime(X86 *c) {
    uint32_t out = arg(c, 1);
    if (out_ok(out, 8))
        wr64(out, ref_now());
    set_eax(c, out ? S_OK : E_POINTER);
}
void Perf_GetQueueTime(X86 *c) {
    uint32_t out = arg(c, 1);
    if (out_ok(out, 8))
        wr64(out, ref_now() + 1000000); // 100 ms ahead
    set_eax(c, out ? S_OK : E_POINTER);
}
DM_OK(Perf_AdjustTime)
void Perf_CloseDown(X86 *c) {
    ComObj *perf = this_kind(c, K_DMPERF);
    if (perf) {
        release_id(perf->dm_default_path);
        release_id(perf->dm_port);
        release_id(perf->dm_tool);
        release_id(perf->dm_dsound);
    }
    set_eax(c, S_OK);
}
// GetResolvedTime(rtTime (8 bytes), prtResolved, dwTimeResolveFlags)
void Perf_GetResolvedTime(X86 *c) {
    uint32_t out = arg(c, 3);
    if (out_ok(out, 8)) {
        wr32(out, arg(c, 1));
        wr32(out + 4, arg(c, 2));
    }
    set_eax(c, out ? S_OK : E_POINTER);
}
DM_FAIL(Perf_MIDIToMusic, E_NOTIMPL)
DM_FAIL(Perf_MusicToMIDI, E_NOTIMPL)
DM_FAIL(Perf_TimeToRhythm, E_NOTIMPL)
DM_FAIL(Perf_RhythmToTime, E_NOTIMPL)
DM_FAIL(Perf_CreateAudioPath, E_NOTIMPL)
// CreateStandardAudioPath(dwType, dwPChannelCount, fActivate, ppNewPath)
void Perf_CreateStandardAudioPath(X86 *c) {
    ComObj *perf = this_kind(c, K_DMPERF);
    uint32_t type = arg(c, 1), pch = arg(c, 2), activate = arg(c, 3), out = arg(c, 4);
    if (!perf || !out_ok(out)) {
        set_eax(c, E_POINTER);
        return;
    }
    if (type != APATH_SHARED_STEREOPLUSREVERB && type != APATH_DYNAMIC_3D &&
        type != APATH_DYNAMIC_MONO && type != APATH_DYNAMIC_STEREO) {
        wr32(out, 0);
        set_eax(c, E_INVALIDARG);
        return;
    }
    ComObj *path = new_path(perf, type, pch);
    path->ap_active = activate != 0;
    wr32(out, com_view(path, IF_DMAUDIOPATH8));
    set_eax(c, S_OK);
}
void Perf_SetDefaultAudioPath(X86 *c) {
    ComObj *perf = this_kind(c, K_DMPERF);
    ComObj *path = com_this(arg(c, 1));
    if (!perf) {
        set_eax(c, E_POINTER);
        return;
    }
    release_id(perf->dm_default_path);
    if (path && path->kind == K_DMAUDIOPATH) {
        com_addref(path);
        perf->dm_default_path = path->id;
    }
    set_eax(c, S_OK);
}
void Perf_GetDefaultAudioPath(X86 *c) {
    ComObj *perf = this_kind(c, K_DMPERF);
    uint32_t out = arg(c, 1);
    if (!perf || !out_ok(out)) {
        set_eax(c, E_POINTER);
        return;
    }
    uint32_t v = hand_out(com_get(perf->dm_default_path), IF_DMAUDIOPATH8);
    wr32(out, v);
    set_eax(c, v ? S_OK : E_FAIL);
}
void perf_destroy(ComObj *perf) {
    release_id(perf->dm_default_path);
    release_id(perf->dm_port);
    release_id(perf->dm_tool);
    release_id(perf->dm_dsound);
}

const ComMethod g_perf8[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"Init", 4, Perf_Init},
    {"PlaySegment", 6, Perf_PlaySegment},
    {"Stop", 5, Perf_Stop},
    {"GetSegmentState", 3, Perf_GetSegmentState},
    {"SetPrepareTime", 2, Perf_SetPrepareTime},
    {"GetPrepareTime", 2, Perf_GetPrepareTime},
    {"SetBumperLength", 2, Perf_SetBumperLength},
    {"GetBumperLength", 2, Perf_GetBumperLength},
    {"SendPMsg", 2, Perf_SendPMsg},
    {"MusicToReferenceTime", 3, Perf_MusicToReferenceTime},
    {"ReferenceToMusicTime", 4, Perf_ReferenceToMusicTime},
    {"IsPlaying", 3, Perf_IsPlaying},
    {"GetTime", 3, Perf_GetTime},
    {"AllocPMsg", 3, Perf_AllocPMsg},
    {"FreePMsg", 2, Perf_FreePMsg},
    {"GetGraph", 2, Perf_GetGraph},
    {"SetGraph", 2, Perf_SetGraph},
    {"SetNotificationHandle", 4, Perf_SetNotificationHandle},
    {"GetNotificationPMsg", 2, Perf_GetNotificationPMsg},
    {"AddNotificationType", 2, Perf_AddNotificationType},
    {"RemoveNotificationType", 2, Perf_RemoveNotificationType},
    {"AddPort", 2, Perf_AddPort},
    {"RemovePort", 2, Perf_RemovePort},
    {"AssignPChannelBlock", 4, Perf_AssignPChannelBlock},
    {"AssignPChannel", 5, Perf_AssignPChannel},
    {"PChannelInfo", 5, Perf_PChannelInfo},
    {"DownloadInstrument", 9, Perf_DownloadInstrument},
    {"Invalidate", 3, Perf_Invalidate},
    {"GetParam", 7, Perf_GetParam},
    {"SetParam", 6, Perf_SetParam},
    {"GetGlobalParam", 4, Perf_GetGlobalParam},
    {"SetGlobalParam", 4, Perf_SetGlobalParam},
    {"GetLatencyTime", 2, Perf_GetLatencyTime},
    {"GetQueueTime", 2, Perf_GetQueueTime},
    {"AdjustTime", 3, Perf_AdjustTime},
    {"CloseDown", 1, Perf_CloseDown},
    {"GetResolvedTime", 5, Perf_GetResolvedTime},
    {"MIDIToMusic", 6, Perf_MIDIToMusic},
    {"MusicToMIDI", 6, Perf_MusicToMIDI},
    {"TimeToRhythm", 7, Perf_TimeToRhythm},
    {"RhythmToTime", 7, Perf_RhythmToTime},
    {"InitAudio", 8, Perf_InitAudio},
    {"PlaySegmentEx", 10, Perf_PlaySegmentEx},
    {"StopEx", 5, Perf_StopEx},
    {"ClonePMsg", 3, Perf_ClonePMsg},
    {"CreateAudioPath", 4, Perf_CreateAudioPath},
    {"CreateStandardAudioPath", 5, Perf_CreateStandardAudioPath},
    {"SetDefaultAudioPath", 2, Perf_SetDefaultAudioPath},
    {"GetDefaultAudioPath", 2, Perf_GetDefaultAudioPath},
    {"GetParamEx", 8, Perf_GetParamEx},
};

ComObj *dmusic_create() {
    return com_new(K_DMUSIC);
}
ComObj *perf_create() {
    return com_new(K_DMPERF);
}

} // namespace

void dmusic_reset() {
    g_clock_origin = 0;
}

void dmusic_register() {
    static bool done = false;
    if (done)
        return;
    done = true;
    const char *dll = "dmusic.dll";
    com_define(IF_DMUSIC8, dll, "IDirectMusic8", g_dmusic, std::size(g_dmusic));
    com_define(IF_DMPERF8, "dmime.dll", "IDirectMusicPerformance8", g_perf8, std::size(g_perf8));
    com_define(IF_DMPORT, dll, "IDirectMusicPort", g_port, std::size(g_port));
    com_define(IF_DMPORTDOWNLOAD, dll, "IDirectMusicPortDownload", g_portdownload,
               std::size(g_portdownload));
    com_define(IF_DMDOWNLOAD, dll, "IDirectMusicDownload", g_download, std::size(g_download));
    com_define(IF_DMAUDIOPATH8, "dmime.dll", "IDirectMusicAudioPath8", g_audiopath,
               std::size(g_audiopath));
    com_define(IF_DMGRAPH, "dmime.dll", "IDirectMusicGraph", g_graph, std::size(g_graph));
    com_define(IF_DMTOOL, "dmime.dll", "IDirectMusicTool", g_tool, std::size(g_tool));

    com_bind(IF_DMUSIC8, K_DMUSIC);
    com_bind(IF_DMPERF8, K_DMPERF);
    com_bind(IF_DMGRAPH, K_DMPERF);
    com_bind(IF_DMPORT, K_DMPORT);
    com_bind(IF_DMPORTDOWNLOAD, K_DMPORT);
    com_bind(IF_DMDOWNLOAD, K_DMDOWNLOAD);
    com_bind(IF_DMAUDIOPATH8, K_DMAUDIOPATH);
    com_bind(IF_DMGRAPH, K_DMAUDIOPATH);
    com_bind(IF_DMTOOL, K_DMTOOL);

    for (const uint8_t *iid : {IID_IDirectMusic_, IID_IDirectMusic2_, IID_IDirectMusic8_})
        com_register_iid(IF_DMUSIC8, iid);
    for (const uint8_t *iid : {IID_IDirectMusicPerformance_, IID_IDirectMusicPerformance2_,
                               IID_IDirectMusicPerformance8_})
        com_register_iid(IF_DMPERF8, iid);
    com_register_iid(IF_DMPORT, IID_IDirectMusicPort_);
    com_register_iid(IF_DMPORTDOWNLOAD, IID_IDirectMusicPortDownload_);
    com_register_iid(IF_DMDOWNLOAD, IID_IDirectMusicDownload_);
    com_register_iid(IF_DMAUDIOPATH8, IID_IDirectMusicAudioPath_);
    com_register_iid(IF_DMGRAPH, IID_IDirectMusicGraph_);
    com_register_iid(IF_DMTOOL, IID_IDirectMusicTool_);
    com_register_iid(IF_DMTOOL, IID_IDirectMusicTool8_);

    com_set_destructor(K_DMPERF, perf_destroy);
    com_set_destructor(K_DMAUDIOPATH, path_destroy);
    com_set_destructor(K_DMDOWNLOAD, download_destroy);

    com_register_class(CLSID_DirectMusic_, "DirectMusic", IF_DMUSIC8, dmusic_create);
    com_register_class(CLSID_DirectMusicPerformance_, "DirectMusicPerformance", IF_DMPERF8,
                       perf_create);
}
