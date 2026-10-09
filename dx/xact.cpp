// xact.cpp - a silent XACT 2 engine (DirectX SDK, August 2007).
//
// A game creates the engine with CoCreateInstance(CLSID_XACTEngine), hands
// it its global settings and then loads sound banks and wave banks and plays
// cues by index. This engine accepts all of it and plays nothing: banks load,
// cue indices resolve, and every cue or wave a game plays has already
// finished, which is what a game sees from a sound that has ended. That keeps
// a game whose audio initialisation must succeed running, silently, until a
// real XACT renderer exists.
//
// The vtable orders and pop counts follow xact.h of the August 2007 SDK
// (XACT 2.9). Bully: Scholarship Edition's own calls confirm the engine's:
// GetRendererDetails at +0x10 with three dwords, Initialize at +0x18 and
// RegisterNotification at +0x3c. Only IXACTEngine is a COM interface; the
// banks, cues and waves are plain interfaces without IUnknown, released with
// Destroy. No notification is ever delivered.
#include "com.h"
#include "dx.h"
#include "../runtime/guest.h"
#include "../runtime/memory.h"

#include <iterator>
#include <string.h>

namespace {

// {962f5027-99be-4692-a468-85802cf8de61}, its debug and auditioning
// variants, and IID_IXACTEngine {e72c1b9a-d717-41c0-81a6-50eb56e80649}.
const uint8_t CLSID_XACTEngine_[16] = {0x27, 0x50, 0x2f, 0x96, 0xbe, 0x99, 0x92, 0x46,
                                       0xa4, 0x68, 0x85, 0x80, 0x2c, 0xf8, 0xde, 0x61};
const uint8_t CLSID_XACTDebugEngine_[16] = {0x1d, 0x2b, 0x6a, 0x02, 0x04, 0xf2, 0x4e, 0x48,
                                            0xab, 0x36, 0xab, 0x2b, 0x66, 0x8f, 0x95, 0x4e};
const uint8_t IID_IXACTEngine_[16] = {0x9a, 0x1b, 0x2c, 0xe7, 0x17, 0xd7, 0xc0, 0x41,
                                      0x81, 0xa6, 0x50, 0xeb, 0x56, 0xe8, 0x06, 0x49};

const uint32_t XACT_CUESTATE_STOPPED = 0x20;
const uint32_t XACT_STATE_PREPARED = 0x04;
const uint16_t XACTINDEX_INVALID = 0xffff;

void ok(X86 *c) {
    set_eax(c, S_OK);
}
void put(uint32_t p, uint32_t v) {
    if (p && gm_valid(p, 4))
        wr32(p, v);
}
void zero_bytes(uint32_t p, uint32_t n) {
    if (p && gm_valid(p, n))
        memset(gm_ptr(p), 0, n);
}

ComObj *create_engine() {
    return com_new(K_XACT);
}

// Hands out a new silent object through `out` as `iface`.
void make(X86 *c, ComIface iface, uint32_t out) {
    if (!out || !gm_valid(out, 4)) {
        set_eax(c, E_INVALIDARG);
        return;
    }
    ComObj *o = com_new(K_XACT);
    uint32_t view = o ? com_view(o, iface) : 0;
    wr32(out, view);
    set_eax(c, view ? S_OK : E_OUTOFMEMORY);
}

void destroy(X86 *c) {
    if (ComObj *o = com_this_arg(c))
        com_release(o);
    set_eax(c, S_OK);
}

// --- IXACTEngine --------------------------------------------------------
void E_GetRendererCount(X86 *c) {
    put(arg(c, 1), 1);
    set_eax(c, S_OK);
}
// XACT_RENDERER_DETAILS: WCHAR rendererID[0xff], WCHAR displayName[0xff],
// BOOL defaultDevice.
void E_GetRendererDetails(X86 *c) {
    uint32_t p = arg(c, 2);
    if (arg(c, 1) != 0 || !p || !gm_valid(p, 1024)) {
        set_eax(c, E_INVALIDARG);
        return;
    }
    zero_bytes(p, 1024);
    const char *id = "{silent}", *name = "Silent output";
    for (uint32_t i = 0; id[i]; ++i)
        wr16(p + 2 * i, (uint16_t)id[i]);
    for (uint32_t i = 0; name[i]; ++i)
        wr16(p + 510 + 2 * i, (uint16_t)name[i]);
    put(p + 1020, 1);
    set_eax(c, S_OK);
}
// WAVEFORMATEXTENSIBLE: 48 kHz stereo float.
void E_GetFinalMixFormat(X86 *c) {
    uint32_t p = arg(c, 1);
    if (!p || !gm_valid(p, 40)) {
        set_eax(c, E_INVALIDARG);
        return;
    }
    zero_bytes(p, 40);
    wr16(p + 0, 0xfffe); // WAVE_FORMAT_EXTENSIBLE
    wr16(p + 2, 2);
    wr32(p + 4, 48000);
    wr32(p + 8, 48000 * 8);
    wr16(p + 12, 8);
    wr16(p + 14, 32);
    wr16(p + 16, 22);
    wr16(p + 18, 32);
    wr32(p + 20, 3); // SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT
    static const uint8_t ieee_float[16] = {0x03, 0, 0, 0,    0, 0,    0x10, 0,
                                           0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
    memcpy(gm_ptr(p + 24), ieee_float, 16);
    set_eax(c, S_OK);
}
void E_CreateSoundBank(X86 *c) {
    make(c, IF_XACT_SOUNDBANK, arg(c, 5));
}
void E_CreateInMemoryWaveBank(X86 *c) {
    make(c, IF_XACT_WAVEBANK, arg(c, 5));
}
void E_CreateStreamingWaveBank(X86 *c) {
    make(c, IF_XACT_WAVEBANK, arg(c, 2));
}
void E_PrepareWave(X86 *c) {
    make(c, IF_XACT_WAVE, arg(c, 7));
}
// PrepareInMemoryWave(this, dwFlags, WAVEBANKENTRY entry (24 bytes by value),
// pdwSeekTable, pbWaveData, dwPlayOffset, nLoopCount, ppWave): 12 dwords.
void E_PrepareInMemoryWave(X86 *c) {
    make(c, IF_XACT_WAVE, arg(c, 11));
}
// PrepareStreamingWave(this, dwFlags, WAVEBANKENTRY (24 bytes),
// XACT_STREAMING_PARAMETERS (12 bytes), dwAlignment, pdwSeekTable,
// dwPlayOffset, nLoopCount, ppWave): 15 dwords.
void E_PrepareStreamingWave(X86 *c) {
    make(c, IF_XACT_WAVE, arg(c, 14));
}
void E_GetCategory(X86 *c) {
    set_eax(c, 0);
}
void E_GetGlobalVariableIndex(X86 *c) {
    set_eax(c, 0);
}
void E_GetGlobalVariable(X86 *c) {
    put(arg(c, 2), 0);
    set_eax(c, S_OK);
}

// --- IXACTSoundBank -----------------------------------------------------
void SB_GetCueIndex(X86 *c) {
    set_eax(c, arg(c, 1) ? 0 : XACTINDEX_INVALID);
}
void SB_GetNumCues(X86 *c) {
    put(arg(c, 1), 1);
    set_eax(c, S_OK);
}
void SB_GetCueProperties(X86 *c) {
    zero_bytes(arg(c, 2), 0x9c); // XACT_CUE_PROPERTIES, packed
    set_eax(c, S_OK);
}
void SB_Prepare(X86 *c) {
    make(c, IF_XACT_CUE, arg(c, 4));
}
// Play's ppCue is optional: a fire-and-forget cue needs no object.
void SB_Play(X86 *c) {
    uint32_t out = arg(c, 4);
    if (out)
        make(c, IF_XACT_CUE, out);
    else
        set_eax(c, S_OK);
}
void state_prepared(X86 *c) {
    put(arg(c, 1), XACT_STATE_PREPARED);
    set_eax(c, S_OK);
}

// --- IXACTWaveBank ------------------------------------------------------
void WB_GetNumWaves(X86 *c) {
    put(arg(c, 1), 1);
    set_eax(c, S_OK);
}
void WB_GetWaveIndex(X86 *c) {
    set_eax(c, arg(c, 1) ? 0 : XACTINDEX_INVALID);
}
void WB_GetWaveProperties(X86 *c) {
    zero_bytes(arg(c, 2), 0x40);
    set_eax(c, S_OK);
}
void WB_Prepare(X86 *c) {
    make(c, IF_XACT_WAVE, arg(c, 5));
}
void WB_Play(X86 *c) {
    make(c, IF_XACT_WAVE, arg(c, 5));
}

// --- IXACTCue and IXACTWave --------------------------------------------
void stopped(X86 *c) {
    put(arg(c, 1), XACT_CUESTATE_STOPPED);
    set_eax(c, S_OK);
}
void C_GetVariableIndex(X86 *c) {
    set_eax(c, 0);
}
void C_GetVariable(X86 *c) {
    put(arg(c, 2), 0);
    set_eax(c, S_OK);
}
void C_GetProperties(X86 *c) {
    put(arg(c, 1), 0);
    set_eax(c, E_FAIL);
}

const ComMethod g_engine[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetRendererCount", 2, E_GetRendererCount},
    {"GetRendererDetails", 3, E_GetRendererDetails},
    {"GetFinalMixFormat", 2, E_GetFinalMixFormat},
    {"Initialize", 2, ok},
    {"ShutDown", 1, ok},
    {"DoWork", 1, ok},
    {"CreateSoundBank", 6, E_CreateSoundBank},
    {"CreateInMemoryWaveBank", 6, E_CreateInMemoryWaveBank},
    {"CreateStreamingWaveBank", 3, E_CreateStreamingWaveBank},
    {"PrepareWave", 8, E_PrepareWave},
    {"PrepareInMemoryWave", 12, E_PrepareInMemoryWave},
    {"PrepareStreamingWave", 15, E_PrepareStreamingWave},
    {"RegisterNotification", 2, ok},
    {"UnRegisterNotification", 2, ok},
    {"GetCategory", 2, E_GetCategory},
    {"Stop", 3, ok},
    {"SetVolume", 3, ok},
    {"Pause", 3, ok},
    {"GetGlobalVariableIndex", 2, E_GetGlobalVariableIndex},
    {"SetGlobalVariable", 3, ok},
    {"GetGlobalVariable", 3, E_GetGlobalVariable},
};
const ComMethod g_soundbank[] = {
    {"GetCueIndex", 2, SB_GetCueIndex},
    {"GetNumCues", 2, SB_GetNumCues},
    {"GetCueProperties", 3, SB_GetCueProperties},
    {"Prepare", 5, SB_Prepare},
    {"Play", 5, SB_Play},
    {"Stop", 3, ok},
    {"Destroy", 1, destroy},
    {"GetState", 2, state_prepared},
};
const ComMethod g_wavebank[] = {
    {"Destroy", 1, destroy},
    {"GetNumWaves", 2, WB_GetNumWaves},
    {"GetWaveIndex", 2, WB_GetWaveIndex},
    {"GetWaveProperties", 3, WB_GetWaveProperties},
    {"Prepare", 6, WB_Prepare},
    {"Play", 6, WB_Play},
    {"Stop", 3, ok},
    {"GetState", 2, state_prepared},
};
const ComMethod g_cue[] = {
    {"Play", 1, ok},
    {"Stop", 2, ok},
    {"GetState", 2, stopped},
    {"Destroy", 1, destroy},
    {"SetMatrixCoefficients", 4, ok},
    {"GetVariableIndex", 2, C_GetVariableIndex},
    {"SetVariable", 3, ok},
    {"GetVariable", 3, C_GetVariable},
    {"Pause", 2, ok},
    {"GetProperties", 2, C_GetProperties},
};
const ComMethod g_wave[] = {
    {"Destroy", 1, destroy},
    {"Play", 1, ok},
    {"Stop", 2, ok},
    {"Pause", 2, ok},
    {"GetState", 2, stopped},
    {"SetPitch", 2, ok},
    {"SetVolume", 2, ok},
    {"SetMatrixCoefficients", 4, ok},
    {"GetProperties", 2, C_GetProperties},
};

} // namespace

void xact_register() {
    static bool done = false;
    if (done)
        return;
    done = true;
    com_define(IF_XACT_ENGINE, "xactengine2_9.dll", "IXACTEngine", g_engine, std::size(g_engine));
    com_define(IF_XACT_SOUNDBANK, "xactengine2_9.dll", "IXACTSoundBank", g_soundbank,
               std::size(g_soundbank));
    com_define(IF_XACT_WAVEBANK, "xactengine2_9.dll", "IXACTWaveBank", g_wavebank,
               std::size(g_wavebank));
    com_define(IF_XACT_CUE, "xactengine2_9.dll", "IXACTCue", g_cue, std::size(g_cue));
    com_define(IF_XACT_WAVE, "xactengine2_9.dll", "IXACTWave", g_wave, std::size(g_wave));
    for (ComIface i :
         {IF_XACT_ENGINE, IF_XACT_SOUNDBANK, IF_XACT_WAVEBANK, IF_XACT_CUE, IF_XACT_WAVE})
        com_bind(i, K_XACT);
    com_register_iid(IF_XACT_ENGINE, IID_IXACTEngine_);
    com_register_class(CLSID_XACTEngine_, "XACTEngine", IF_XACT_ENGINE, create_engine);
    com_register_class(CLSID_XACTDebugEngine_, "XACTDebugEngine", IF_XACT_ENGINE, create_engine);
}
