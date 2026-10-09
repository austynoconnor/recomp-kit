// dshow_video.cpp - DirectShow movies played into a renderer the game wrote.
//
// Metal Gear Solid 2 builds its movie graph by hand: it adds its own
// renderer (a CBaseRenderer that copies RGB24 pictures into a buffer the game
// draws from), adds the movie file with AddSourceFilter and connects the
// source's "Output" pin to the renderer's "In" pin. On Windows, Connect
// inserts the MPEG-1 splitter and the MPEG Video Decoder between them. Here
// the source filter is the decoder: it decodes the MPEG-1 elementary stream
// with FFmpeg (mf::Mpeg1Stream) and its output pin offers RGB24 straight to
// the renderer, through samples from an allocator of its own.
//
// The renderer is guest code; every call into it goes through guest_call.
// It has no reference clock, so a sample it receives while running is drawn
// at once; pictures are therefore paced here, by the guest clock, from the
// frame pump. Samples are only delivered while the graph runs: a
// CBaseRenderer that is paused holds the sample and waits, which would stall
// the guest thread the pump runs on.
//
// Interfaces served here (strmif.h slot orders):
//   IBaseFilter (the source), IPin (its output pin), IMemAllocator, and
//   IMediaSample. A guest object is called through its vtable: IBaseFilter
//   Stop 4, Pause 5, Run 6, JoinFilterGraph 13; IPin ReceiveConnection 4;
//   IMemInputPin NotifyAllocator 4, Receive 6.
#include "dshow_video.h"

#include "com.h"
#include "dx.h"
#include "mf_media.h"
#include "../runtime/memory.h"
#include "../runtime/win32.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

#define GUID_BYTES(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7)                                        \
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

const uint8_t IID_IBaseFilter_[16] =
    GUID_BYTES(0x56a86895, 0x0ad4, 0x11ce, 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70);
const uint8_t IID_IMediaFilter_[16] =
    GUID_BYTES(0x56a86899, 0x0ad4, 0x11ce, 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70);
const uint8_t IID_IPin_[16] =
    GUID_BYTES(0x56a86891, 0x0ad4, 0x11ce, 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70);
const uint8_t IID_IMemAllocator_[16] =
    GUID_BYTES(0x56a8689c, 0x0ad4, 0x11ce, 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70);
const uint8_t IID_IMediaSample_[16] =
    GUID_BYTES(0x56a8689a, 0x0ad4, 0x11ce, 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70);
const uint8_t IID_IMemInputPin_[16] =
    GUID_BYTES(0x56a8689d, 0x0ad4, 0x11ce, 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70);
// CLSID_CMpegVideoCodec, which the source reports as its class: it stands in
// for the decoder.
const uint8_t CLSID_MpegVideoCodec_[16] =
    GUID_BYTES(0xfeb50740, 0x7bef, 0x11ce, 0x9b, 0xd9, 0x00, 0x00, 0xe2, 0x02, 0x59, 0x9c);
const uint8_t MEDIATYPE_Video_[16] =
    GUID_BYTES(0x73646976, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
const uint8_t MEDIASUBTYPE_RGB24_[16] =
    GUID_BYTES(0xe436eb7d, 0x524f, 0x11ce, 0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70);
const uint8_t FORMAT_VideoInfo_[16] =
    GUID_BYTES(0x05589f80, 0xc356, 0x11ce, 0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a);

const uint32_t VFW_E_NOT_FOUND = 0x80040216u;
const uint32_t VFW_E_NOT_CONNECTED = 0x80040209u;
const uint32_t VFW_E_ALREADY_CONNECTED = 0x80040204u;
const uint32_t VFW_E_NOT_COMMITTED = 0x80040211u;
const uint32_t VFW_E_SAMPLE_TIME_NOT_SET = 0x80040249u;
const uint32_t EC_COMPLETE_ = 1;
const uint32_t PINDIR_OUTPUT = 1;
const uint32_t AM_MEDIA_TYPE_SIZE = 0x48;
const uint32_t VIDEOINFOHEADER_SIZE = 88;

enum : uint32_t { kStopped = 0, kPaused = 1, kRunning = 2 };

// Everything one graph's movie needs. Ids are ComObj ids (the object vector
// may move, so pointers are never kept); guest pointers are the renderer's.
struct Movie {
    uint32_t graph_id = 0;
    uint32_t graph_view = 0;
    uint32_t renderer = 0; // guest IBaseFilter*, referenced
    uint32_t source = 0, pin = 0, allocator = 0, sample = 0;
    uint32_t peer = 0;     // guest IPin* of the renderer, referenced while connected
    uint32_t peer_mem = 0; // its IMemInputPin*, referenced while connected
    uint32_t mt = 0;       // the connection's AM_MEDIA_TYPE, guest heap
    uint32_t buffer = 0, buffer_bytes = 0;
    std::string name;
    mf::Mpeg1Stream stream;
    mf::VideoFrame frame;
    bool have_frame = false;
    int64_t sent = -1; // index of the last picture handed to the renderer
    uint32_t state = kStopped;
    uint64_t base = 0;   // media time at run_ms
    uint32_t run_ms = 0; // guest clock when the graph last ran or seeked
    bool committed = false;
    bool completed = false; // EC_COMPLETE posted for this run
    int32_t sample_actual = 0;
};

std::map<uint32_t, std::unique_ptr<Movie>> &movies() {
    static std::map<uint32_t, std::unique_ptr<Movie>> m;
    return m;
}

Movie *movie_of_graph(uint32_t graph_id) {
    auto it = movies().find(graph_id);
    return it == movies().end() ? nullptr : it->second.get();
}

// The movie a source, pin, allocator or sample belongs to (dsh_owner holds
// the graph id).
Movie *movie_of(const ComObj *o) {
    return o ? movie_of_graph(o->dsh_owner) : nullptr;
}

Movie &movie_for(uint32_t graph_id) {
    auto &slot = movies()[graph_id];
    if (!slot) {
        slot = std::make_unique<Movie>();
        slot->graph_id = graph_id;
    }
    return *slot;
}

// --- Guest objects -----------------------------------------------------------
uint32_t guest_slot(uint32_t obj, int slot) {
    if (!obj || !gm_valid(obj, 4))
        return 0;
    uint32_t vt = rd32(obj);
    if (!vt || !gm_valid(vt + 4u * (uint32_t)slot, 4))
        return 0;
    return rd32(vt + 4u * (uint32_t)slot);
}
// RECOMP_DSHOW_TRACE=1 logs the first calls into the game's renderer.
bool dsv_trace() {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("RECOMP_DSHOW_TRACE");
        on = v && *v && *v != '0';
    }
    return on != 0;
}
uint32_t g_dsv_traced = 0;

uint32_t guest_method(X86 *c, uint32_t obj, int slot, std::initializer_list<uint32_t> rest) {
    uint32_t fn = guest_slot(obj, slot);
    if (!fn)
        return 0x80004005u;
    std::vector<uint32_t> a;
    a.push_back(obj);
    a.insert(a.end(), rest.begin(), rest.end());
    uint32_t hr = guest_call(c, fn, a.data(), (int)a.size());
    if (dsv_trace() && g_dsv_traced++ < 200)
        LOGW("dshow trace: guest %08x slot %d (fn %08x) -> %08x", obj, slot, fn, hr);
    return hr;
}
void guest_addref(X86 *c, uint32_t obj) {
    if (obj)
        guest_method(c, obj, 1, {});
}
void guest_release(X86 *c, uint32_t obj) {
    if (obj)
        guest_method(c, obj, 2, {});
}

uint64_t ms_to_time(uint32_t ms) {
    return (uint64_t)ms * 10000u;
}

uint64_t movie_duration(const Movie &m) {
    const mf::Mpeg1Info &i = m.stream.info();
    return i.fps > 0 ? (uint64_t)((double)i.pictures * 1e7 / i.fps) : 0;
}

uint64_t movie_position(const Movie &m) {
    uint64_t t = m.base;
    if (m.state == kRunning)
        t += ms_to_time(host_millis() - m.run_ms);
    uint64_t end = movie_duration(m);
    return t > end ? end : t;
}

// Writes the RGB24 VIDEOINFOHEADER media type the source offers.
uint32_t make_media_type(const Movie &m) {
    const mf::Mpeg1Info &i = m.stream.info();
    uint32_t mt = heap_alloc(AM_MEDIA_TYPE_SIZE, true);
    uint32_t vih = heap_alloc(VIDEOINFOHEADER_SIZE, true);
    if (!mt || !vih)
        return 0;
    const uint32_t stride = ((uint32_t)i.width * 3u + 3u) & ~3u;
    const uint32_t image = stride * (uint32_t)i.height;
    memcpy(gm_ptr(mt), MEDIATYPE_Video_, 16);
    memcpy(gm_ptr(mt + 0x10), MEDIASUBTYPE_RGB24_, 16);
    wr32(mt + 0x20, 1);     // bFixedSizeSamples
    wr32(mt + 0x24, 0);     // bTemporalCompression
    wr32(mt + 0x28, image); // lSampleSize
    memcpy(gm_ptr(mt + 0x2c), FORMAT_VideoInfo_, 16);
    wr32(mt + 0x40, VIDEOINFOHEADER_SIZE);
    wr32(mt + 0x44, vih);
    wr32(vih + 0x08, (uint32_t)i.width); // rcSource and rcTarget
    wr32(vih + 0x0c, (uint32_t)i.height);
    wr32(vih + 0x18, (uint32_t)i.width);
    wr32(vih + 0x1c, (uint32_t)i.height);
    const uint64_t per_frame = i.fps > 0 ? (uint64_t)(1e7 / i.fps) : 0;
    wr32(vih + 0x28, (uint32_t)per_frame);
    wr32(vih + 0x2c, (uint32_t)(per_frame >> 32));
    wr32(vih + 0x30, 40);                 // biSize
    wr32(vih + 0x34, (uint32_t)i.width);  // biWidth
    wr32(vih + 0x38, (uint32_t)i.height); // biHeight: positive, bottom-up
    wr16(vih + 0x3c, 1);                  // biPlanes
    wr16(vih + 0x3e, 24);                 // biBitCount
    wr32(vih + 0x40, 0);                  // BI_RGB
    wr32(vih + 0x44, image);              // biSizeImage
    return mt;
}

void free_media_type(uint32_t mt) {
    if (!mt)
        return;
    if (uint32_t fmt = rd32(mt + 0x44))
        heap_free(fmt);
    heap_free(mt);
}

// Copies `mt` (ours) into the caller's AM_MEDIA_TYPE, with a format block of
// its own that the caller frees with CoTaskMemFree.
bool copy_media_type(uint32_t dst, uint32_t mt) {
    if (!dst || !gm_valid(dst, AM_MEDIA_TYPE_SIZE) || !mt)
        return false;
    memcpy(gm_ptr(dst), gm_ptr(mt), AM_MEDIA_TYPE_SIZE);
    uint32_t fmt = heap_alloc(VIDEOINFOHEADER_SIZE, false);
    if (!fmt)
        return false;
    memcpy(gm_ptr(fmt), gm_ptr(rd32(mt + 0x44)), VIDEOINFOHEADER_SIZE);
    wr32(dst + 0x44, fmt);
    return true;
}

// The picture as the renderer takes it: RGB24, bottom-up rows padded to four
// bytes, written into the sample's buffer.
void write_picture(Movie &m) {
    const mf::VideoFrame &f = m.frame;
    const uint32_t stride = ((uint32_t)f.width * 3u + 3u) & ~3u;
    const uint32_t need = stride * (uint32_t)f.height;
    if (!m.buffer || need > m.buffer_bytes)
        return;
    uint8_t *dst = gm_ptr(m.buffer);
    for (int y = 0; y < f.height; ++y) {
        uint8_t *row = dst + (size_t)(f.height - 1 - y) * stride;
        const uint32_t *src = f.argb.data() + (size_t)y * (size_t)f.width;
        for (int x = 0; x < f.width; ++x) {
            const uint32_t p = src[x]; // 0xAARRGGBB
            row[3 * x + 0] = (uint8_t)(p & 0xff);
            row[3 * x + 1] = (uint8_t)((p >> 8) & 0xff);
            row[3 * x + 2] = (uint8_t)((p >> 16) & 0xff);
        }
    }
    m.sample_actual = (int32_t)need;
}

} // namespace

// ===========================================================================
// The source filter: IBaseFilter.
// ===========================================================================
namespace {

Movie *this_movie(X86 *c, ComIface iface) {
    return movie_of(com_this_arg(c, iface));
}

void out_obj(X86 *c, uint32_t out, uint32_t id, ComIface iface) {
    ComObj *o = com_get(id);
    if (!out || !gm_valid(out, 4)) {
        com_ret(c, E_POINTER);
        return;
    }
    uint32_t v = o ? com_view(o, iface) : 0;
    wr32(out, v);
    if (!v) {
        com_ret(c, E_FAIL);
        return;
    }
    com_addref(com_get(id));
    com_ret(c, S_OK);
}

void SF_GetClassID(X86 *c) {
    uint32_t out = arg(c, 1);
    if (!out || !gm_valid(out, 16)) {
        com_ret(c, E_POINTER);
        return;
    }
    memcpy(gm_ptr(out), CLSID_MpegVideoCodec_, 16);
    com_ret(c, S_OK);
}
// The graph drives the renderer and paces the pictures; the source's own
// state only answers GetState.
void SF_Stop(X86 *c) {
    com_ret(c, S_OK);
}
void SF_Pause(X86 *c) {
    com_ret(c, S_OK);
}
void SF_Run(X86 *c) {
    com_ret(c, S_OK);
}
void SF_GetState(X86 *c) {
    Movie *m = this_movie(c, IF_DSV_FILTER);
    uint32_t out = arg(c, 2);
    if (!out || !gm_valid(out, 4)) {
        com_ret(c, E_POINTER);
        return;
    }
    wr32(out, m ? m->state : kStopped);
    com_ret(c, S_OK);
}
void SF_SetSyncSource(X86 *c) {
    com_ret(c, S_OK);
}
void SF_GetSyncSource(X86 *c) {
    uint32_t out = arg(c, 1);
    if (out && gm_valid(out, 4))
        wr32(out, 0);
    com_ret(c, S_OK);
}
DX_STUB(SF_EnumPins, E_NOTIMPL)
// FindPin(Id, ppPin): the one pin is "Output".
void SF_FindPin(X86 *c) {
    Movie *m = this_movie(c, IF_DSV_FILTER);
    uint32_t id = arg(c, 1), out = arg(c, 2);
    std::string name;
    for (uint32_t i = 0; id && gm_valid(id + 2 * i, 2) && i < 64; ++i) {
        uint16_t ch = rd16(id + 2 * i);
        if (!ch)
            break;
        name += (char)ch;
    }
    if (!m || name != "Output") {
        if (out && gm_valid(out, 4))
            wr32(out, 0);
        com_ret(c, VFW_E_NOT_FOUND);
        return;
    }
    out_obj(c, out, m->pin, IF_DSV_PIN);
}
// QueryFilterInfo(FILTER_INFO*): WCHAR achName[128], IFilterGraph *pGraph.
void SF_QueryFilterInfo(X86 *c) {
    Movie *m = this_movie(c, IF_DSV_FILTER);
    uint32_t out = arg(c, 1);
    if (!out || !gm_valid(out, 260)) {
        com_ret(c, E_POINTER);
        return;
    }
    gm_zero(out, 260);
    gm_put_wstr(out, "MPEG Video Decoder", 128);
    if (m && m->graph_view) {
        wr32(out + 256, m->graph_view);
        com_addref(com_get(m->graph_id));
    }
    com_ret(c, S_OK);
}
void SF_JoinFilterGraph(X86 *c) {
    com_ret(c, S_OK);
}
DX_STUB(SF_QueryVendorInfo, E_NOTIMPL)

const ComMethod g_filter[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetClassID", 2, SF_GetClassID},
    {"Stop", 1, SF_Stop},
    {"Pause", 1, SF_Pause},
    {"Run", 3, SF_Run},
    {"GetState", 3, SF_GetState},
    {"SetSyncSource", 2, SF_SetSyncSource},
    {"GetSyncSource", 2, SF_GetSyncSource},
    {"EnumPins", 2, SF_EnumPins},
    {"FindPin", 3, SF_FindPin},
    {"QueryFilterInfo", 2, SF_QueryFilterInfo},
    {"JoinFilterGraph", 3, SF_JoinFilterGraph},
    {"QueryVendorInfo", 2, SF_QueryVendorInfo},
};

// ===========================================================================
// The output pin: IPin.
// ===========================================================================
DX_STUB(OP_Connect, E_NOTIMPL)
DX_STUB(OP_ReceiveConnection, 0x8000FFFFu)
void OP_Disconnect(X86 *c) {
    Movie *m = this_movie(c, IF_DSV_PIN);
    com_ret(c, m && m->peer ? S_OK : S_FALSE);
}
void OP_ConnectedTo(X86 *c) {
    Movie *m = this_movie(c, IF_DSV_PIN);
    uint32_t out = arg(c, 1);
    if (!out || !gm_valid(out, 4)) {
        com_ret(c, E_POINTER);
        return;
    }
    if (!m || !m->peer) {
        wr32(out, 0);
        com_ret(c, VFW_E_NOT_CONNECTED);
        return;
    }
    guest_addref(c, m->peer);
    wr32(out, m->peer);
    com_ret(c, S_OK);
}
void OP_ConnectionMediaType(X86 *c) {
    Movie *m = this_movie(c, IF_DSV_PIN);
    uint32_t out = arg(c, 1);
    if (!m || !m->peer) {
        if (out && gm_valid(out, AM_MEDIA_TYPE_SIZE))
            gm_zero(out, AM_MEDIA_TYPE_SIZE);
        com_ret(c, VFW_E_NOT_CONNECTED);
        return;
    }
    com_ret(c, copy_media_type(out, m->mt) ? S_OK : E_POINTER);
}
// QueryPinInfo(PIN_INFO*): IBaseFilter *pFilter, PIN_DIRECTION dir,
// WCHAR achName[128].
void OP_QueryPinInfo(X86 *c) {
    Movie *m = this_movie(c, IF_DSV_PIN);
    uint32_t out = arg(c, 1);
    if (!out || !gm_valid(out, 264)) {
        com_ret(c, E_POINTER);
        return;
    }
    gm_zero(out, 264);
    ComObj *src = m ? com_get(m->source) : nullptr;
    uint32_t v = src ? com_view(src, IF_DSV_FILTER) : 0;
    if (v) {
        com_addref(com_get(m->source));
        wr32(out, v);
    }
    wr32(out + 4, PINDIR_OUTPUT);
    gm_put_wstr(out + 8, "Output", 128);
    com_ret(c, S_OK);
}
void OP_QueryDirection(X86 *c) {
    uint32_t out = arg(c, 1);
    if (!out || !gm_valid(out, 4)) {
        com_ret(c, E_POINTER);
        return;
    }
    wr32(out, PINDIR_OUTPUT);
    com_ret(c, S_OK);
}
void OP_QueryId(X86 *c) {
    uint32_t out = arg(c, 1);
    if (!out || !gm_valid(out, 4)) {
        com_ret(c, E_POINTER);
        return;
    }
    uint32_t s = heap_alloc(16, true);
    if (s)
        gm_put_wstr(s, "Output", 8);
    wr32(out, s);
    com_ret(c, s ? S_OK : E_OUTOFMEMORY);
}
void OP_QueryAccept(X86 *c) {
    uint32_t mt = arg(c, 1);
    bool ok = mt && gm_valid(mt, AM_MEDIA_TYPE_SIZE) &&
              memcmp(gm_ptr(mt), MEDIATYPE_Video_, 16) == 0 &&
              memcmp(gm_ptr(mt + 0x10), MEDIASUBTYPE_RGB24_, 16) == 0;
    com_ret(c, ok ? S_OK : S_FALSE);
}
DX_STUB(OP_EnumMediaTypes, E_NOTIMPL)
DX_STUB(OP_QueryInternalConnections, E_NOTIMPL)
void OP_Ok(X86 *c) {
    com_ret(c, S_OK);
}

const ComMethod g_pin[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"Connect", 3, OP_Connect},
    {"ReceiveConnection", 3, OP_ReceiveConnection},
    {"Disconnect", 1, OP_Disconnect},
    {"ConnectedTo", 2, OP_ConnectedTo},
    {"ConnectionMediaType", 2, OP_ConnectionMediaType},
    {"QueryPinInfo", 2, OP_QueryPinInfo},
    {"QueryDirection", 2, OP_QueryDirection},
    {"QueryId", 2, OP_QueryId},
    {"QueryAccept", 2, OP_QueryAccept},
    {"EnumMediaTypes", 2, OP_EnumMediaTypes},
    {"QueryInternalConnections", 3, OP_QueryInternalConnections},
    {"EndOfStream", 1, OP_Ok},
    {"BeginFlush", 1, OP_Ok},
    {"EndFlush", 1, OP_Ok},
    {"NewSegment", 7, OP_Ok},
};

// ===========================================================================
// IMemAllocator: one buffer, one sample, owned by the source.
// ALLOCATOR_PROPERTIES: cBuffers, cbBuffer, cbAlign, cbPrefix.
// ===========================================================================
void write_props(uint32_t out, const Movie &m) {
    if (!out || !gm_valid(out, 16))
        return;
    wr32(out, 1);
    wr32(out + 4, m.buffer_bytes);
    wr32(out + 8, 1);
    wr32(out + 12, 0);
}
void AL_SetProperties(X86 *c) {
    Movie *m = this_movie(c, IF_DSV_ALLOCATOR);
    if (!m) {
        com_ret(c, E_FAIL);
        return;
    }
    write_props(arg(c, 2), *m);
    com_ret(c, S_OK);
}
void AL_GetProperties(X86 *c) {
    Movie *m = this_movie(c, IF_DSV_ALLOCATOR);
    if (!m) {
        com_ret(c, E_FAIL);
        return;
    }
    write_props(arg(c, 1), *m);
    com_ret(c, S_OK);
}
void AL_Commit(X86 *c) {
    if (Movie *m = this_movie(c, IF_DSV_ALLOCATOR))
        m->committed = true;
    com_ret(c, S_OK);
}
void AL_Decommit(X86 *c) {
    if (Movie *m = this_movie(c, IF_DSV_ALLOCATOR))
        m->committed = false;
    com_ret(c, S_OK);
}
// GetBuffer(ppBuffer, pStart, pEnd, dwFlags)
void AL_GetBuffer(X86 *c) {
    Movie *m = this_movie(c, IF_DSV_ALLOCATOR);
    uint32_t out = arg(c, 1);
    if (!out || !gm_valid(out, 4)) {
        com_ret(c, E_POINTER);
        return;
    }
    if (!m || !m->committed) {
        wr32(out, 0);
        com_ret(c, VFW_E_NOT_COMMITTED);
        return;
    }
    out_obj(c, out, m->sample, IF_DSV_SAMPLE);
}
void AL_ReleaseBuffer(X86 *c) {
    com_ret(c, S_OK);
}

const ComMethod g_allocator[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"SetProperties", 3, AL_SetProperties},
    {"GetProperties", 2, AL_GetProperties},
    {"Commit", 1, AL_Commit},
    {"Decommit", 1, AL_Decommit},
    {"GetBuffer", 5, AL_GetBuffer},
    {"ReleaseBuffer", 2, AL_ReleaseBuffer},
};

// ===========================================================================
// IMediaSample: the picture buffer. Untimed, so a renderer without a clock
// draws it at once; every picture is a key frame.
// ===========================================================================
void MS_GetPointer(X86 *c) {
    Movie *m = this_movie(c, IF_DSV_SAMPLE);
    uint32_t out = arg(c, 1);
    if (!out || !gm_valid(out, 4)) {
        com_ret(c, E_POINTER);
        return;
    }
    wr32(out, m ? m->buffer : 0);
    com_ret(c, m ? S_OK : 0x80004005u);
}
void MS_GetSize(X86 *c) {
    Movie *m = this_movie(c, IF_DSV_SAMPLE);
    set_eax(c, m ? m->buffer_bytes : 0);
}
void MS_GetTime(X86 *c) {
    com_ret(c, VFW_E_SAMPLE_TIME_NOT_SET);
}
void MS_IsSyncPoint(X86 *c) {
    com_ret(c, S_OK);
}
void MS_IsFalse(X86 *c) {
    com_ret(c, S_FALSE);
}
void MS_SetOk(X86 *c) {
    com_ret(c, S_OK);
}
void MS_GetActualDataLength(X86 *c) {
    Movie *m = this_movie(c, IF_DSV_SAMPLE);
    set_eax(c, m ? (uint32_t)m->sample_actual : 0);
}
void MS_SetActualDataLength(X86 *c) {
    Movie *m = this_movie(c, IF_DSV_SAMPLE);
    uint32_t n = arg(c, 1);
    if (!m || n > m->buffer_bytes) {
        com_ret(c, 0x80070057u);
        return;
    }
    m->sample_actual = (int32_t)n;
    com_ret(c, S_OK);
}
// GetMediaType(ppMediaType): S_FALSE, the type has not changed.
void MS_GetMediaType(X86 *c) {
    uint32_t out = arg(c, 1);
    if (out && gm_valid(out, 4))
        wr32(out, 0);
    com_ret(c, S_FALSE);
}
void MS_GetMediaTime(X86 *c) {
    com_ret(c, 0x80040251u); // VFW_E_MEDIA_TIME_NOT_SET
}

const ComMethod g_sample[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetPointer", 2, MS_GetPointer},
    {"GetSize", 1, MS_GetSize},
    {"GetTime", 3, MS_GetTime},
    {"SetTime", 3, MS_SetOk},
    {"IsSyncPoint", 1, MS_IsSyncPoint},
    {"SetSyncPoint", 2, MS_SetOk},
    {"IsPreroll", 1, MS_IsFalse},
    {"SetPreroll", 2, MS_SetOk},
    {"GetActualDataLength", 1, MS_GetActualDataLength},
    {"SetActualDataLength", 2, MS_SetActualDataLength},
    {"GetMediaType", 2, MS_GetMediaType},
    {"SetMediaType", 2, MS_SetOk},
    {"IsDiscontinuity", 1, MS_IsFalse},
    {"SetDiscontinuity", 2, MS_SetOk},
    {"GetMediaTime", 3, MS_GetMediaTime},
    {"SetMediaTime", 3, MS_SetOk},
};

} // namespace

// ===========================================================================
// The graph's side.
// ===========================================================================
namespace {

void owned_release(uint32_t &id) {
    if (ComObj *o = com_get(id)) {
        o->dsh_owner = 0;
        com_release(o);
    }
    id = 0;
}

// Hands the next due picture to the renderer. Pictures decoded on the way to
// a later one are skipped, as a late decoder would drop them.
void deliver(X86 *c, Movie &m) {
    if (m.state != kRunning || !m.peer_mem || !m.committed)
        return;
    const mf::Mpeg1Info &info = m.stream.info();
    if (info.fps <= 0)
        return;
    const uint64_t now = movie_position(m);
    if (!m.completed && now >= movie_duration(m)) {
        m.completed = true;
        dshow_post_graph_event(m.graph_id, EC_COMPLETE_);
    }
    int64_t due = (int64_t)((double)now * info.fps / 1e7);
    if (due > info.pictures - 1)
        due = info.pictures - 1;
    if (due <= m.sent)
        return;
    // Decode up to the due picture; a few at most per tick, so a long seek
    // forward spreads over several frames instead of stalling one.
    int budget = 16;
    while (m.stream.decoded() <= due && budget-- > 0) {
        if (!m.stream.next(&m.frame))
            break;
        m.have_frame = true;
    }
    if (!m.have_frame)
        return;
    const int64_t index = m.stream.decoded() - 1;
    if (index <= m.sent)
        return;
    write_picture(m);
    ComObj *sample = com_get(m.sample);
    uint32_t view = sample ? com_view(sample, IF_DSV_SAMPLE) : 0;
    if (!view)
        return;
    m.sent = index;
    com_addref(com_get(m.sample));
    guest_method(c, m.peer_mem, 6, {view}); // IMemInputPin::Receive
    com_release(com_get(m.sample));
}

void reposition(Movie &m, uint64_t t) {
    const uint64_t end = movie_duration(m);
    if (t > end)
        t = end;
    const mf::Mpeg1Info &info = m.stream.info();
    const int64_t want = info.fps > 0 ? (int64_t)((double)t * info.fps / 1e7) : 0;
    if (want < m.stream.decoded()) {
        m.stream.rewind();
        m.have_frame = false;
    }
    m.sent = want - 1;
    m.base = t;
    m.run_ms = host_millis();
    m.completed = false;
}

} // namespace

namespace dsv {

bool has_movie(uint32_t graph_id) {
    Movie *m = movie_of_graph(graph_id);
    return m && m->stream.is_open();
}

bool is_movie_file(const std::string &host_path) {
    FILE *f = fopen(host_path.c_str(), "rb");
    if (!f)
        return false;
    uint8_t head[4] = {0};
    size_t n = fread(head, 1, 4, f);
    fclose(f);
    return n == 4 && head[0] == 0 && head[1] == 0 && head[2] == 1 && head[3] == 0xb3;
}

uint32_t add_filter(X86 *c, uint32_t graph_id, uint32_t graph_view, uint32_t filter,
                    uint32_t name) {
    if (!filter || !gm_valid(filter, 4))
        return E_POINTER;
    if (com_this(filter)) // one of ours: the source joins with AddSourceFilter
        return S_OK;
    Movie &m = movie_for(graph_id);
    m.graph_view = graph_view;
    if (m.renderer && m.renderer != filter) {
        guest_method(c, m.renderer, 13, {0, 0}); // the old one leaves the graph
        guest_release(c, m.renderer);
    }
    if (m.renderer != filter) {
        guest_addref(c, filter);
        m.renderer = filter;
    }
    return guest_method(c, filter, 13, {graph_view, name}); // JoinFilterGraph
}

uint32_t add_source(X86 *c, uint32_t graph_id, const std::string &guest_path,
                    const std::string &host_path, uint32_t out) {
    (void)c;
    if (out && gm_valid(out, 4))
        wr32(out, 0);
    // Without a decoder, the file is not read at all: on the web every byte
    // of it would be a download.
    if (!mf::mpeg1_decoder_available()) {
        log_once("dshow.nodecoder", "dshow: this build has no MPEG-1 video decoder; movies are "
                                    "skipped");
        return 0x80040241u; // VFW_E_CANNOT_RENDER
    }
    FILE *f = fopen(host_path.c_str(), "rb");
    if (!f)
        return 0x80070002u; // HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
    std::vector<uint8_t> bytes;
    uint8_t chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
        bytes.insert(bytes.end(), chunk, chunk + n);
    fclose(f);
    mf::mpeg1_fix_aspect(bytes);
    Movie &m = movie_for(graph_id);
    std::string why;
    if (!m.stream.open(std::move(bytes), &why)) {
        LOGW("dshow: AddSourceFilter(%s): %s", guest_path.c_str(), why.c_str());
        return 0x80040241u; // VFW_E_CANNOT_RENDER
    }
    m.name = guest_path;
    const mf::Mpeg1Info &info = m.stream.info();
    const uint32_t stride = ((uint32_t)info.width * 3u + 3u) & ~3u;
    m.buffer_bytes = stride * (uint32_t)info.height;
    if (!m.buffer)
        m.buffer = heap_alloc(m.buffer_bytes, true);
    if (!m.buffer)
        return E_OUTOFMEMORY;
    owned_release(m.source);
    owned_release(m.pin);
    owned_release(m.allocator);
    owned_release(m.sample);
    // Each object made before the next com_new: the object table may move.
    m.source = com_new(K_DSV_SOURCE)->id;
    com_get(m.source)->dsh_owner = graph_id;
    m.pin = com_new(K_DSV_PIN)->id;
    com_get(m.pin)->dsh_owner = graph_id;
    m.allocator = com_new(K_DSV_ALLOCATOR)->id;
    com_get(m.allocator)->dsh_owner = graph_id;
    m.sample = com_new(K_DSV_SAMPLE)->id;
    com_get(m.sample)->dsh_owner = graph_id;
    m.sent = -1;
    m.base = 0;
    m.state = kStopped;
    m.completed = false;
    m.have_frame = false;
    LOGV("dshow: AddSourceFilter(%s): MPEG-1 %dx%d, %.3f fps, %lld pictures", guest_path.c_str(),
         info.width, info.height, info.fps, (long long)info.pictures);
    log_once("dshow.movie", "dshow: playing MPEG-1 movies into the game's own renderer");
    if (!out || !gm_valid(out, 4))
        return E_POINTER;
    uint32_t v = com_view(com_get(m.source), IF_DSV_FILTER);
    if (!v)
        return E_OUTOFMEMORY;
    com_addref(com_get(m.source));
    wr32(out, v);
    return S_OK;
}

uint32_t connect(X86 *c, uint32_t graph_id, uint32_t out, uint32_t in) {
    Movie *m = movie_of_graph(graph_id);
    ComObj *pin = com_this(out, IF_DSV_PIN);
    if (!m || !pin || pin->id != m->pin)
        return S_FALSE;
    if (!in || !gm_valid(in, 4) || com_this(in))
        return E_POINTER;
    if (m->peer)
        return VFW_E_ALREADY_CONNECTED;
    if (!m->mt)
        m->mt = make_media_type(*m);
    uint32_t pin_view = com_view(com_get(m->pin), IF_DSV_PIN);
    if (!m->mt || !pin_view)
        return E_OUTOFMEMORY;
    uint32_t hr = guest_method(c, in, 4, {pin_view, m->mt}); // ReceiveConnection
    if (hr & 0x80000000u) {
        LOGW("dshow: %s: the renderer refused RGB24 (hr 0x%08x)", m->name.c_str(), hr);
        return hr;
    }
    m->peer = in;
    guest_addref(c, in);
    uint32_t iid = heap_alloc(20, false);
    if (!iid)
        return E_OUTOFMEMORY;
    memcpy(gm_ptr(iid), IID_IMemInputPin_, 16);
    wr32(iid + 16, 0);
    hr = guest_method(c, in, 0, {iid, iid + 16}); // QueryInterface(IID_IMemInputPin)
    m->peer_mem = (hr & 0x80000000u) ? 0 : rd32(iid + 16);
    heap_free(iid);
    if (!m->peer_mem) {
        LOGW("dshow: %s: the renderer's pin has no IMemInputPin", m->name.c_str());
        return E_NOINTERFACE;
    }
    uint32_t alloc_view = com_view(com_get(m->allocator), IF_DSV_ALLOCATOR);
    hr = guest_method(c, m->peer_mem, 4, {alloc_view, 0}); // NotifyAllocator
    m->committed = true;
    return (hr & 0x80000000u) ? hr : S_OK;
}

uint32_t run(X86 *c, uint32_t graph_id) {
    Movie *m = movie_of_graph(graph_id);
    if (!m)
        return E_FAIL;
    if (m->state == kRunning)
        return S_OK;
    m->committed = true;
    m->run_ms = host_millis();
    m->state = kRunning;
    if (m->renderer)
        guest_method(c, m->renderer, 6, {0, 0}); // Run(tStart 0)
    return S_OK;
}

uint32_t pause(X86 *c, uint32_t graph_id) {
    Movie *m = movie_of_graph(graph_id);
    if (!m)
        return E_FAIL;
    m->base = movie_position(*m);
    m->state = kPaused;
    if (m->renderer)
        guest_method(c, m->renderer, 5, {}); // Pause
    return S_OK;
}

uint32_t stop(X86 *c, uint32_t graph_id) {
    Movie *m = movie_of_graph(graph_id);
    if (!m)
        return E_FAIL;
    if (m->state != kStopped) {
        m->base = movie_position(*m);
        m->state = kStopped;
        if (m->renderer)
            guest_method(c, m->renderer, 4, {}); // Stop
    }
    return S_OK;
}

uint32_t state(uint32_t graph_id) {
    Movie *m = movie_of_graph(graph_id);
    return m ? m->state : kStopped;
}

uint64_t duration(uint32_t graph_id) {
    Movie *m = movie_of_graph(graph_id);
    return m ? movie_duration(*m) : 0;
}

uint64_t position(uint32_t graph_id) {
    Movie *m = movie_of_graph(graph_id);
    return m ? movie_position(*m) : 0;
}

void seek(uint32_t graph_id, uint64_t t) {
    if (Movie *m = movie_of_graph(graph_id))
        reposition(*m, t);
}

void pump(X86 *c) {
    std::vector<uint32_t> ids;
    for (auto &e : movies())
        ids.push_back(e.first);
    for (uint32_t id : ids) // a Receive may change the map
        if (Movie *m = movie_of_graph(id))
            deliver(c, *m);
}

void forget(uint32_t graph_id) {
    auto it = movies().find(graph_id);
    if (it == movies().end())
        return;
    Movie &m = *it->second;
    owned_release(m.source);
    owned_release(m.pin);
    owned_release(m.allocator);
    owned_release(m.sample);
    free_media_type(m.mt);
    if (m.buffer)
        heap_free(m.buffer);
    // The renderer's references are left: there is no guest thread to call
    // Release on here, and the game frees its renderer with the graph.
    movies().erase(it);
}

void reset() {
    movies().clear();
}

void register_interfaces() {
    com_define(IF_DSV_FILTER, "QUARTZ.dll", "IBaseFilter", g_filter, std::size(g_filter));
    com_define(IF_DSV_PIN, "QUARTZ.dll", "IPin", g_pin, std::size(g_pin));
    com_define(IF_DSV_ALLOCATOR, "QUARTZ.dll", "IMemAllocator", g_allocator,
               std::size(g_allocator));
    com_define(IF_DSV_SAMPLE, "QUARTZ.dll", "IMediaSample", g_sample, std::size(g_sample));
    com_bind(IF_DSV_FILTER, K_DSV_SOURCE);
    com_bind(IF_DSV_PIN, K_DSV_PIN);
    com_bind(IF_DSV_ALLOCATOR, K_DSV_ALLOCATOR);
    com_bind(IF_DSV_SAMPLE, K_DSV_SAMPLE);
    com_register_iid(IF_DSV_FILTER, IID_IBaseFilter_);
    com_register_iid(IF_DSV_FILTER, IID_IMediaFilter_);
    com_register_iid(IF_DSV_PIN, IID_IPin_);
    com_register_iid(IF_DSV_ALLOCATOR, IID_IMemAllocator_);
    com_register_iid(IF_DSV_SAMPLE, IID_IMediaSample_);
}

} // namespace dsv
