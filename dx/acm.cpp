// acm.cpp - the MSACM32 conversion stream, decoding to 16-bit PCM.
//
// A game hands the Audio Compression Manager compressed blocks and gets PCM
// back; the codec itself is a system driver it never names. This runtime has
// no drivers, so the stream converts the three formats games of the period
// feed it - PCM itself (any 8/16-bit layout to the destination's), IMA ADPCM
// (WAVE_FORMAT_IMA_ADPCM, 0x11) and Microsoft ADPCM (WAVE_FORMAT_ADPCM, 0x2) -
// and refuses anything else with ACMERR_NOTPOSSIBLE, as a system without the
// codec would. Nothing here plays sound; the game passes the PCM on to
// DirectSound or waveOut itself.
//
// Guest layouts used (all little-endian):
//   WAVEFORMATEX: wFormatTag +0, nChannels +2, nSamplesPerSec +4,
//     nAvgBytesPerSec +8, nBlockAlign +12, wBitsPerSample +14, cbSize +16,
//     then for both ADPCMs wSamplesPerBlock +18; MS ADPCM adds wNumCoef +20
//     and its coefficient pairs from +22.
//   ACMSTREAMHEADER: cbStruct +0, fdwStatus +4, dwUser +8, pbSrc +12,
//     cbSrcLength +16, cbSrcLengthUsed +20, dwSrcUser +24, pbDst +28,
//     cbDstLength +32, cbDstLengthUsed +36, dwDstUser +40, reserved +44.
#include "dx.h"
#include "../runtime/guest.h"
#include "../runtime/imports.h"
#include "../runtime/memory.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <map>
#include <vector>

namespace {

constexpr uint32_t MMSYSERR_NOERROR = 0, MMSYSERR_INVALHANDLE = 5, MMSYSERR_INVALFLAG = 10,
                   MMSYSERR_INVALPARAM = 11, ACMERR_NOTPOSSIBLE = 512;
constexpr uint32_t ACMSTREAMHEADER_STATUSF_DONE = 0x00010000u,
                   ACMSTREAMHEADER_STATUSF_PREPARED = 0x00020000u;
constexpr uint32_t ACM_STREAMOPENF_QUERY = 0x1u;
constexpr uint32_t ACM_STREAMSIZEF_DESTINATION = 0x1u;
constexpr uint16_t TAG_PCM = 1, TAG_MS_ADPCM = 2, TAG_IMA_ADPCM = 0x11;

struct Format {
    uint16_t tag = 0, channels = 0, block_align = 0, bits = 0, samples_per_block = 0;
    uint32_t rate = 0;
    std::vector<int16_t> coef; // MS ADPCM: pairs
};

struct Stream {
    Format src, dst;
};

std::map<uint32_t, Stream> &streams() {
    static std::map<uint32_t, Stream> s;
    return s;
}
uint32_t g_next_stream = 0x00ac0001u;

const int16_t kMsDefaultCoef[14] = {256, 0, 512, -256, 0, 0, 192, 64, 240, 0, 460, -208, 392, -232};

bool read_format(uint32_t p, Format &f) {
    if (!p || !gm_valid(p, 16))
        return false;
    f.tag = rd16(p);
    f.channels = rd16(p + 2);
    f.rate = rd32(p + 4);
    f.block_align = rd16(p + 12);
    f.bits = rd16(p + 14);
    uint16_t extra = f.tag == TAG_PCM ? 0 : rd16(p + 16);
    if (f.channels < 1 || f.channels > 2 || !f.rate)
        return false;
    if (f.tag == TAG_PCM) {
        f.block_align = (uint16_t)(f.channels * f.bits / 8); // some callers leave it 0
        return f.bits == 8 || f.bits == 16;
    }
    if (f.tag != TAG_MS_ADPCM && f.tag != TAG_IMA_ADPCM)
        return true; // known to be unconvertible; the caller says so
    if (f.bits != 4 || f.block_align < 7u * f.channels || extra < 2)
        return false;
    f.samples_per_block = rd16(p + 18);
    if (f.tag == TAG_IMA_ADPCM) {
        uint32_t expect = (f.block_align - 4u * f.channels) * 2u / f.channels + 1u;
        if (!f.samples_per_block)
            f.samples_per_block = (uint16_t)expect;
        return f.samples_per_block <= expect;
    }
    uint16_t ncoef = extra >= 4 ? rd16(p + 20) : 0;
    if (ncoef) {
        if (ncoef > 64 || extra < 4u + 4u * ncoef)
            return false;
        for (uint16_t i = 0; i < 2 * ncoef; ++i)
            f.coef.push_back((int16_t)rd16(p + 22 + 2 * i));
    } else {
        f.coef.assign(std::begin(kMsDefaultCoef), std::end(kMsDefaultCoef));
    }
    uint32_t expect = (f.block_align - 7u * f.channels) * 2u / f.channels + 2u;
    if (!f.samples_per_block)
        f.samples_per_block = (uint16_t)expect;
    return f.samples_per_block <= expect;
}

bool convertible(const Format &s, const Format &d) {
    if (d.tag != TAG_PCM || s.rate != d.rate || s.channels != d.channels)
        return false;
    if (s.tag == TAG_PCM)
        return true;
    return (s.tag == TAG_IMA_ADPCM || s.tag == TAG_MS_ADPCM) && d.bits == 16;
}

int16_t clamp16(int32_t v) {
    return (int16_t)std::min(32767, std::max(-32768, v));
}

// IMA ADPCM, one block: a 4-byte header per channel (predictor, step index),
// then 4-byte words alternating by channel, eight nibbles each, low first.
void decode_ima(const uint8_t *in, const Format &f, int16_t *out) {
    static const int kIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
    static const int kStep[89] = {
        7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,   21,    23,
        25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,   73,    80,
        88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,  253,   279,
        307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,  876,   963,
        1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749, 3024,  3327,
        3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487,
        12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
    const unsigned ch = f.channels, n = f.samples_per_block;
    for (unsigned c = 0; c < ch; ++c) {
        int pred = (int16_t)(in[4 * c] | (in[4 * c + 1] << 8));
        int index = std::min(88, std::max(0, (int)in[4 * c + 2]));
        out[c] = (int16_t)pred;
        const uint8_t *data = in + 4 * ch;
        for (unsigned s = 1; s < n; ++s) {
            unsigned k = s - 1;                            // nibble number in this channel
            unsigned word = (k / 8) * ch + c, nib = k % 8; // words interleave by channel
            uint8_t byte = data[word * 4 + nib / 2];
            int code = nib & 1 ? byte >> 4 : byte & 0xf;
            int step = kStep[index], diff = step >> 3;
            if (code & 4)
                diff += step;
            if (code & 2)
                diff += step >> 1;
            if (code & 1)
                diff += step >> 2;
            pred = clamp16(code & 8 ? pred - diff : pred + diff);
            index = std::min(88, std::max(0, index + kIndex[code]));
            out[s * ch + c] = (int16_t)pred;
        }
    }
}

// Microsoft ADPCM, one block: per channel a predictor index, then delta,
// sample1 and sample2 as int16s; output starts sample2, sample1, then the
// nibbles high first, alternating channels for stereo.
void decode_ms(const uint8_t *in, const Format &f, int16_t *out) {
    static const int kAdapt[16] = {230, 230, 230, 230, 307, 409, 512, 614,
                                   768, 614, 512, 409, 307, 230, 230, 230};
    const unsigned ch = f.channels, n = f.samples_per_block, ncoef = (unsigned)f.coef.size() / 2;
    int c1[2], c2[2], delta[2], s1[2], s2[2];
    const uint8_t *p = in;
    for (unsigned c = 0; c < ch; ++c) {
        unsigned idx = std::min<unsigned>(*p++, ncoef - 1);
        c1[c] = f.coef[2 * idx];
        c2[c] = f.coef[2 * idx + 1];
    }
    auto rd = [&p]() {
        int v = (int16_t)(p[0] | (p[1] << 8));
        p += 2;
        return v;
    };
    for (unsigned c = 0; c < ch; ++c)
        delta[c] = rd();
    for (unsigned c = 0; c < ch; ++c)
        s1[c] = rd();
    for (unsigned c = 0; c < ch; ++c)
        s2[c] = rd();
    for (unsigned c = 0; c < ch; ++c) {
        out[c] = (int16_t)s2[c];
        out[ch + c] = (int16_t)s1[c];
    }
    for (unsigned k = 0; k < (n - 2) * ch; ++k) {
        unsigned c = k % ch;
        uint8_t byte = p[k / 2];
        int code = k & 1 ? byte & 0xf : byte >> 4;
        int signed_code = code & 8 ? code - 16 : code;
        int predicted = (s1[c] * c1[c] + s2[c] * c2[c]) / 256;
        int sample = clamp16(predicted + signed_code * delta[c]);
        s2[c] = s1[c];
        s1[c] = sample;
        delta[c] = std::max(16, kAdapt[code] * delta[c] / 256);
        out[2 * ch + k] = (int16_t)sample;
    }
}

uint32_t frame_bytes(const Format &f) {
    return (uint32_t)f.channels * f.bits / 8u;
}

// Bytes of destination the given source bytes make, and the reverse, in whole
// blocks (ADPCM) or whole frames (PCM).
uint32_t dst_for_src(const Stream &s, uint32_t src) {
    if (s.src.tag == TAG_PCM)
        return src / frame_bytes(s.src) * frame_bytes(s.dst);
    return src / s.src.block_align * s.src.samples_per_block * frame_bytes(s.dst);
}
uint32_t src_for_dst(const Stream &s, uint32_t dst) {
    if (s.src.tag == TAG_PCM)
        return (dst + frame_bytes(s.dst) - 1) / frame_bytes(s.dst) * frame_bytes(s.src);
    uint32_t per_block = s.src.samples_per_block * frame_bytes(s.dst);
    return (dst + per_block - 1) / per_block * s.src.block_align;
}

Stream *stream_get(uint32_t has) {
    auto it = streams().find(has);
    return it == streams().end() ? nullptr : &it->second;
}

// acmStreamOpen(phas, had, pwfxSrc, pwfxDst, pwfltr, callback, instance, flags)
void acmStreamOpen(X86 *c) {
    uint32_t phas = arg(c, 0), flags = arg(c, 7);
    Stream s;
    if (!read_format(arg(c, 2), s.src) || !read_format(arg(c, 3), s.dst) || arg(c, 4)) {
        set_eax(c, arg(c, 4) ? ACMERR_NOTPOSSIBLE : MMSYSERR_INVALPARAM);
        return;
    }
    if (!convertible(s.src, s.dst)) {
        log_once("acmStreamOpen-unsupported", "acmStreamOpen: no codec for format 0x%x -> 0x%x",
                 s.src.tag, s.dst.tag);
        set_eax(c, ACMERR_NOTPOSSIBLE);
        return;
    }
    if (flags & ACM_STREAMOPENF_QUERY) {
        set_eax(c, MMSYSERR_NOERROR);
        return;
    }
    if (!phas || !gm_valid(phas, 4)) {
        set_eax(c, MMSYSERR_INVALPARAM);
        return;
    }
    uint32_t h = g_next_stream++;
    streams()[h] = s;
    wr32(phas, h);
    set_eax(c, MMSYSERR_NOERROR);
}

void acmStreamClose(X86 *c) {
    set_eax(c, streams().erase(arg(c, 0)) ? MMSYSERR_NOERROR : MMSYSERR_INVALHANDLE);
}

// acmStreamSize(has, cbInput, pdwOutputBytes, flags)
void acmStreamSize(X86 *c) {
    Stream *s = stream_get(arg(c, 0));
    uint32_t in = arg(c, 1), out = arg(c, 2), flags = arg(c, 3);
    if (!s) {
        set_eax(c, MMSYSERR_INVALHANDLE);
        return;
    }
    if (!out || !gm_valid(out, 4) || (flags & ~ACM_STREAMSIZEF_DESTINATION)) {
        set_eax(c, flags & ~ACM_STREAMSIZEF_DESTINATION ? MMSYSERR_INVALFLAG : MMSYSERR_INVALPARAM);
        return;
    }
    uint32_t n = flags & ACM_STREAMSIZEF_DESTINATION ? src_for_dst(*s, in) : dst_for_src(*s, in);
    wr32(out, n);
    set_eax(c, n || !in ? MMSYSERR_NOERROR : ACMERR_NOTPOSSIBLE);
}

bool header_ok(uint32_t h) {
    return h && gm_valid(h, 84) && rd32(h) >= 84;
}

void acmStreamPrepareHeader(X86 *c) {
    uint32_t h = arg(c, 1);
    if (!stream_get(arg(c, 0)) || !header_ok(h)) {
        set_eax(c, stream_get(arg(c, 0)) ? MMSYSERR_INVALPARAM : MMSYSERR_INVALHANDLE);
        return;
    }
    wr32(h + 4, rd32(h + 4) | ACMSTREAMHEADER_STATUSF_PREPARED | ACMSTREAMHEADER_STATUSF_DONE);
    set_eax(c, MMSYSERR_NOERROR);
}

void acmStreamUnprepareHeader(X86 *c) {
    uint32_t h = arg(c, 1);
    if (!stream_get(arg(c, 0)) || !header_ok(h)) {
        set_eax(c, stream_get(arg(c, 0)) ? MMSYSERR_INVALPARAM : MMSYSERR_INVALHANDLE);
        return;
    }
    wr32(h + 4, rd32(h + 4) & ~ACMSTREAMHEADER_STATUSF_PREPARED);
    set_eax(c, MMSYSERR_NOERROR);
}

// acmStreamConvert(has, pash, flags): converts every whole block (or frame)
// of the source that fits in the destination, synchronously, and marks the
// header done. A partial block at the end is left unused, as the system
// codecs leave it for the next call.
void acmStreamConvert(X86 *c) {
    Stream *s = stream_get(arg(c, 0));
    uint32_t h = arg(c, 1);
    if (!s) {
        set_eax(c, MMSYSERR_INVALHANDLE);
        return;
    }
    if (!header_ok(h) || !(rd32(h + 4) & ACMSTREAMHEADER_STATUSF_PREPARED)) {
        set_eax(c, header_ok(h) ? 0x201u /* ACMERR_UNPREPARED */ : MMSYSERR_INVALPARAM);
        return;
    }
    uint32_t src = rd32(h + 12), src_len = rd32(h + 16), dst = rd32(h + 28), dst_len = rd32(h + 32);
    if (!gm_valid(src, src_len) || !gm_valid(dst, dst_len)) {
        set_eax(c, MMSYSERR_INVALPARAM);
        return;
    }
    uint32_t used_src = 0, used_dst = 0;
    const uint32_t out_frame = frame_bytes(s->dst);
    if (s->src.tag == TAG_PCM) {
        const uint32_t in_frame = frame_bytes(s->src);
        uint32_t frames = std::min(src_len / in_frame, dst_len / out_frame);
        for (uint32_t i = 0; i < frames * s->src.channels; ++i) {
            int v = s->src.bits == 8 ? ((int)rd8(src + i) - 128) << 8 : (int16_t)rd16(src + 2 * i);
            if (s->dst.bits == 8)
                wr8(dst + i, (uint8_t)((v >> 8) + 128));
            else
                wr16(dst + 2 * i, (uint16_t)v);
        }
        used_src = frames * in_frame;
        used_dst = frames * out_frame;
    } else {
        const uint32_t block_out = s->src.samples_per_block * out_frame;
        std::vector<int16_t> pcm((size_t)s->src.samples_per_block * s->src.channels);
        while (used_src + s->src.block_align <= src_len && used_dst + block_out <= dst_len) {
            const uint8_t *in = g_mem + src + used_src;
            if (s->src.tag == TAG_IMA_ADPCM)
                decode_ima(in, s->src, pcm.data());
            else
                decode_ms(in, s->src, pcm.data());
            memcpy(g_mem + dst + used_dst, pcm.data(), block_out);
            used_src += s->src.block_align;
            used_dst += block_out;
        }
    }
    wr32(h + 20, used_src);
    wr32(h + 36, used_dst);
    wr32(h + 4, rd32(h + 4) | ACMSTREAMHEADER_STATUSF_DONE);
    set_eax(c, MMSYSERR_NOERROR);
}

// acmFormatSuggest(had, pwfxSrc, pwfxDst, cbwfxDst, flags): 16-bit PCM at
// the source's rate and channel count, which every format here converts to.
void acmFormatSuggest(X86 *c) {
    Format f;
    uint32_t out = arg(c, 2), cap = arg(c, 3);
    if (!read_format(arg(c, 1), f) || !out || cap < 16 || !gm_valid(out, 16)) {
        set_eax(c, MMSYSERR_INVALPARAM);
        return;
    }
    Format pcm;
    pcm.tag = TAG_PCM;
    pcm.channels = f.channels;
    pcm.rate = f.rate;
    pcm.bits = 16;
    pcm.block_align = (uint16_t)(2 * f.channels);
    if (!convertible(f, pcm)) {
        set_eax(c, ACMERR_NOTPOSSIBLE);
        return;
    }
    wr16(out, TAG_PCM);
    wr16(out + 2, pcm.channels);
    wr32(out + 4, pcm.rate);
    wr32(out + 8, pcm.rate * pcm.block_align);
    wr16(out + 12, pcm.block_align);
    wr16(out + 14, 16);
    if (cap >= 18)
        wr16(out + 16, 0);
    set_eax(c, MMSYSERR_NOERROR);
}

const ImportShim g_acm_shims[] = {
    {"MSACM32.dll", "acmStreamOpen", 8, acmStreamOpen},
    {"MSACM32.dll", "acmStreamClose", 2, acmStreamClose},
    {"MSACM32.dll", "acmStreamSize", 4, acmStreamSize},
    {"MSACM32.dll", "acmStreamPrepareHeader", 3, acmStreamPrepareHeader},
    {"MSACM32.dll", "acmStreamUnprepareHeader", 3, acmStreamUnprepareHeader},
    {"MSACM32.dll", "acmStreamConvert", 3, acmStreamConvert},
    {"MSACM32.dll", "acmFormatSuggest", 5, acmFormatSuggest},
};

} // namespace

void acm_register() {
    streams().clear();
    imports_register(g_acm_shims, std::size(g_acm_shims));
}
