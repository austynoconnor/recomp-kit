// vorbisfile.cpp - libvorbisfile's decoding calls (ov_open_callbacks, ov_read,
// ov_info, ov_pcm_total, ov_seekable, ov_clear), decoded by stb_vorbis.
//
// Fallout: New Vegas keeps its voices as Ogg Vorbis in its archives and
// decodes them with the libvorbisfile.dll it ships, handing ov_open_callbacks
// the four reader callbacks of its own archive stream. The game's
// OggVorbis_File is guest memory the caller owns; this keeps the decoder on
// the host, keyed by that address, and never touches the struct's fields.
//
// At open the whole stream is read through the guest's read callback (a
// voice line is tens to hundreds of KB), so decoding never calls back into
// the guest. ov_read hands out 16-bit little-endian signed samples, the only
// form the game asks for; another width or byte order fails the read.
#include "dx.h"
#include "../runtime/guest.h"
#include "../runtime/imports.h"
#include "../runtime/memory.h"

#include <string.h>

#include <iterator>
#include <map>
#include <vector>

#define STB_VORBIS_NO_STDIO 1
#define STB_VORBIS_NO_PUSHDATA_API 1
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#include "../third_party/stb/stb_vorbis.c"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace {

const int32_t OV_EREAD = -128, OV_EFAULT = -129, OV_EINVAL = -131, OV_ENOTVORBIS = -132;

struct Stream {
    std::vector<uint8_t> bytes;
    stb_vorbis *v = nullptr;
    int channels = 0;
    uint32_t rate = 0;
    uint32_t datasource = 0, close_cb = 0;
    uint32_t info = 0; // guest vorbis_info, allocated on first ov_info
};

std::map<uint32_t, Stream> &streams() {
    static auto *m = new std::map<uint32_t, Stream>();
    return *m;
}

void forget(uint32_t vf) {
    auto it = streams().find(vf);
    if (it == streams().end())
        return;
    if (it->second.v)
        stb_vorbis_close(it->second.v);
    if (it->second.info)
        heap_free(it->second.info);
    streams().erase(it);
}

// ov_open_callbacks(datasource, vf, initial, ibytes, {read, seek, close, tell}).
// The callbacks struct is passed by value: four dwords on the stack.
void ov_open_callbacks(X86 *c) {
    uint32_t datasource = arg(c, 0), vf = arg(c, 1), initial = arg(c, 2), ibytes = arg(c, 3);
    uint32_t read_cb = arg(c, 4), close_cb = arg(c, 6);
    if (!vf || !read_cb) {
        set_eax(c, (uint32_t)OV_EINVAL);
        return;
    }
    forget(vf);
    Stream s;
    s.datasource = datasource;
    s.close_cb = close_cb;
    if (initial && ibytes && gm_valid(initial, ibytes))
        s.bytes.assign(g_mem + initial, g_mem + initial + ibytes);
    const uint32_t chunk = 64 * 1024;
    uint32_t buf = heap_alloc(chunk, false);
    if (!buf) {
        set_eax(c, (uint32_t)OV_EFAULT);
        return;
    }
    for (;;) {
        // size_t read(void *ptr, size_t size, size_t nmemb, void *datasource)
        uint32_t args[4] = {buf, 1, chunk, datasource};
        uint32_t got = guest_call(c, read_cb, args, 4);
        if (got == 0 || got > chunk)
            break;
        s.bytes.insert(s.bytes.end(), g_mem + buf, g_mem + buf + got);
        if (s.bytes.size() > 256u * 1024 * 1024)
            break;
    }
    heap_free(buf);
    int err = 0;
    s.v = stb_vorbis_open_memory(s.bytes.data(), (int)s.bytes.size(), &err, nullptr);
    if (!s.v) {
        LOGV("vorbisfile: stream of %zu bytes did not open (stb_vorbis error %d)", s.bytes.size(),
             err);
        set_eax(c, (uint32_t)(s.bytes.empty() ? OV_EREAD : OV_ENOTVORBIS));
        return;
    }
    stb_vorbis_info info = stb_vorbis_get_info(s.v);
    s.channels = info.channels;
    s.rate = info.sample_rate;
    streams()[vf] = std::move(s);
    // libvorbisfile reads the data source from the struct's first field.
    if (gm_valid(vf, 4))
        wr32(vf, datasource);
    set_eax(c, 0);
}

// ov_read(vf, buffer, length, bigendianp, word, sgned, int *bitstream)
void ov_read(X86 *c) {
    auto it = streams().find(arg(c, 0));
    uint32_t buf = arg(c, 1), length = arg(c, 2), big = arg(c, 3), word = arg(c, 4);
    uint32_t sgned = arg(c, 5), bitstream = arg(c, 6);
    if (it == streams().end() || !it->second.v || big || word != 2 || !sgned ||
        !gm_valid(buf, length)) {
        set_eax(c, (uint32_t)OV_EINVAL);
        return;
    }
    Stream &s = it->second;
    int shorts = (int)(length / 2) / s.channels * s.channels;
    if (shorts <= 0) {
        set_eax(c, 0);
        return;
    }
    std::vector<short> pcm((size_t)shorts);
    int frames = stb_vorbis_get_samples_short_interleaved(s.v, s.channels, pcm.data(), shorts);
    uint32_t bytes = (uint32_t)frames * (uint32_t)s.channels * 2;
    memcpy(g_mem + buf, pcm.data(), bytes);
    if (bitstream && gm_valid(bitstream, 4))
        wr32(bitstream, 0);
    set_eax(c, bytes);
}

// ov_info(vf, link): vorbis_info is version, channels, rate, bitrate_upper,
// bitrate_nominal, bitrate_lower, bitrate_window, codec_setup.
void ov_info(X86 *c) {
    auto it = streams().find(arg(c, 0));
    if (it == streams().end()) {
        set_eax(c, 0);
        return;
    }
    Stream &s = it->second;
    if (!s.info) {
        s.info = heap_alloc(32, true);
        if (!s.info) {
            set_eax(c, 0);
            return;
        }
        wr32(s.info + 4, (uint32_t)s.channels);
        wr32(s.info + 8, s.rate);
    }
    set_eax(c, s.info);
}

// ov_pcm_total(vf, i): samples per channel, as a 64-bit value in EDX:EAX.
void ov_pcm_total(X86 *c) {
    auto it = streams().find(arg(c, 0));
    if (it == streams().end() || !it->second.v) {
        set_eax(c, (uint32_t)OV_EINVAL);
        c->r[R_EDX] = 0xffffffffu;
        return;
    }
    uint32_t n = stb_vorbis_stream_length_in_samples(it->second.v);
    set_eax(c, n);
    c->r[R_EDX] = 0;
}

void ov_seekable(X86 *c) {
    set_eax(c, streams().count(arg(c, 0)) ? 1u : 0u);
}

// ov_clear(vf): closes the data source through the guest's close callback.
void ov_clear(X86 *c) {
    uint32_t vf = arg(c, 0);
    auto it = streams().find(vf);
    if (it != streams().end()) {
        uint32_t close_cb = it->second.close_cb, ds = it->second.datasource;
        forget(vf);
        if (close_cb)
            guest_call(c, close_cb, ds);
    }
    set_eax(c, 0);
}

const ImportShim shims[] = {
    {"libvorbisfile.dll", "ov_open_callbacks", ARGC_CDECL, ov_open_callbacks},
    {"libvorbisfile.dll", "ov_read", ARGC_CDECL, ov_read},
    {"libvorbisfile.dll", "ov_info", ARGC_CDECL, ov_info},
    {"libvorbisfile.dll", "ov_pcm_total", ARGC_CDECL, ov_pcm_total},
    {"libvorbisfile.dll", "ov_seekable", ARGC_CDECL, ov_seekable},
    {"libvorbisfile.dll", "ov_clear", ARGC_CDECL, ov_clear},
};

} // namespace

void vorbisfile_register() {
    imports_register(shims, std::size(shims));
}

void vorbisfile_reset() {
    while (!streams().empty())
        forget(streams().begin()->first);
}
