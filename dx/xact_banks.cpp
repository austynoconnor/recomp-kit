// xact_banks.cpp - the XACT 2 file formats; see xact_banks.h.
#include "xact_banks.h"

#include <math.h>
#include <string.h>

namespace xact {
namespace {

// A bounds-checked little-endian reader. Every read past the end sets `bad`
// and yields zero, so a parser checks once at the end instead of everywhere.
struct Reader {
    const uint8_t *d;
    size_t n;
    bool bad = false;
    bool has(size_t at, size_t len) {
        if (at > n || len > n - at) {
            bad = true;
            return false;
        }
        return true;
    }
    uint8_t u8(size_t at) {
        return has(at, 1) ? d[at] : 0;
    }
    uint16_t u16(size_t at) {
        return has(at, 2) ? (uint16_t)(d[at] | d[at + 1] << 8) : 0;
    }
    int16_t s16(size_t at) {
        return (int16_t)u16(at);
    }
    uint32_t u32(size_t at) {
        return has(at, 4) ? (uint32_t)d[at] | (uint32_t)d[at + 1] << 8 | (uint32_t)d[at + 2] << 16 |
                                (uint32_t)d[at + 3] << 24
                          : 0;
    }
    float f32(size_t at) {
        uint32_t v = u32(at);
        float f;
        memcpy(&f, &v, 4);
        return f;
    }
    // `count` NUL-terminated names in a row, as the name tables hold them.
    std::vector<std::string> names(size_t at, size_t count) {
        std::vector<std::string> out;
        for (size_t i = 0; i < count; ++i) {
            if (!has(at, 1))
                break;
            const void *end = memchr(d + at, 0, n - at);
            size_t len = end ? (size_t)((const uint8_t *)end - (d + at)) : n - at;
            out.emplace_back((const char *)d + at, len);
            at += len + 1;
        }
        if (out.size() < count) {
            bad = true;
            out.resize(count);
        }
        return out;
    }
    std::string fixed(size_t at, size_t len) {
        if (!has(at, len))
            return {};
        size_t l = 0;
        while (l < len && d[at + l])
            ++l;
        return std::string((const char *)d + at, l);
    }
};

const uint32_t NONE = 0xffffffff;

bool fail(std::string *error, const char *why) {
    if (error)
        *error = why;
    return false;
}

} // namespace

float RpcCurve::eval(float v) const {
    if (x.empty())
        return 0;
    if (v <= x.front())
        return y.front();
    for (size_t i = 1; i < x.size(); ++i) {
        if (v <= x[i]) {
            float span = x[i] - x[i - 1];
            float t = span > 0 ? (v - x[i - 1]) / span : 1;
            return y[i - 1] + (y[i] - y[i - 1]) * t;
        }
    }
    return y.back();
}

const RpcCurve *Settings::curve(uint32_t code) const {
    for (const RpcCurve &c : curves)
        if (c.code == code)
            return &c;
    return nullptr;
}
int Settings::find_category(const std::string &name) const {
    for (size_t i = 0; i < categories.size(); ++i)
        if (categories[i].name == name)
            return (int)i;
    return -1;
}
int Settings::find_variable(const std::string &name) const {
    for (size_t i = 0; i < variables.size(); ++i)
        if (variables[i].name == name)
            return (int)i;
    return -1;
}
int SoundBank::find_cue(const std::string &name) const {
    for (size_t i = 0; i < cues.size(); ++i)
        if (cues[i].name == name)
            return (int)i;
    return -1;
}

// The authoring tool's volume byte. 0xb4 is unity; the fit is the one the
// format's readers agree on, good to a tenth of a decibel over the range a
// sound designer uses.
float volume_byte_db(uint8_t v) {
    const double a = -96.0, b = 0.432254984608615, c = 80.1748600297963, d = 67.7385212334047;
    double db = (a - d) / (1 + pow(v / c, b)) + d;
    return (float)db;
}

// XGSF, format 42: the header is 18 bytes of version and timestamp, a
// platform byte, then the counts and the offsets of each table.
bool parse_settings(const uint8_t *data, size_t n, Settings *out, std::string *error) {
    Reader r{data, n};
    if (n < 0x4d || memcmp(data, "XGSF", 4))
        return fail(error, "not an XACT global settings file");
    uint16_t cats = r.u16(0x13), vars = r.u16(0x15), rpcs = r.u16(0x1b);
    uint32_t cats_at = r.u32(0x21), vars_at = r.u32(0x25);
    uint32_t cat_names = r.u32(0x39), var_names = r.u32(0x3d), rpcs_at = r.u32(0x41);
    Settings s;
    std::vector<std::string> cn =
        cats && cat_names != NONE ? r.names(cat_names, cats) : std::vector<std::string>(cats);
    for (uint16_t i = 0; i < cats; ++i) {
        size_t at = cats_at + 10u * i; // instances, fades, flags, parent, volume, visibility
        Category c;
        c.name = cn[i];
        c.parent = r.u16(at + 6);
        c.volume_db = volume_byte_db(r.u8(at + 8));
        s.categories.push_back(c);
    }
    std::vector<std::string> vn =
        vars && var_names != NONE ? r.names(var_names, vars) : std::vector<std::string>(vars);
    for (uint16_t i = 0; i < vars; ++i) {
        size_t at = vars_at + 13u * i;
        Variable v;
        v.name = vn[i];
        v.flags = r.u8(at);
        v.initial = r.f32(at + 1);
        v.min = r.f32(at + 5);
        v.max = r.f32(at + 9);
        s.variables.push_back(v);
    }
    size_t at = rpcs_at;
    for (uint16_t i = 0; rpcs_at != NONE && i < rpcs && !r.bad; ++i) {
        RpcCurve c;
        c.code = (uint32_t)at;
        c.variable = r.u16(at);
        uint8_t points = r.u8(at + 2);
        c.parameter = r.u16(at + 3);
        for (uint8_t p = 0; p < points; ++p) {
            c.x.push_back(r.f32(at + 5 + 9u * p));
            c.y.push_back(r.f32(at + 9 + 9u * p));
        }
        at += 5 + 9u * points;
        s.curves.push_back(c);
    }
    if (r.bad)
        return fail(error, "XACT global settings are truncated");
    *out = std::move(s);
    return true;
}

namespace {

// One clip's events. Each event is a 6-byte header - the event id in the low
// five bits of a dword, then a random time offset - and a body whose shape
// the id decides. Only the play-wave events matter here: 1 plays one wave,
// 3 picks among several, and 4 and 6 are those two with a pitch and volume
// variation range.
bool read_clip(Reader &r, size_t at, Cue *cue) {
    uint8_t events = r.u8(at);
    size_t p = at + 1;
    for (uint8_t e = 0; e < events && !r.bad; ++e) {
        uint32_t id = r.u32(p) & 0x1f;
        p += 6;
        switch (id) {
        case 1:
        case 4: {
            Track t;
            t.wave = r.u16(p + 2);
            t.wave_bank = r.u8(p + 4);
            t.loops = r.u8(p + 5);
            if (id == 4) {
                t.pitch_min = r.s16(p + 10);
                t.pitch_max = r.s16(p + 12);
            }
            cue->tracks.push_back(t);
            p += id == 1 ? 10 : 34;
            break;
        }
        case 3:
        case 6: {
            uint8_t loops = r.u8(p + 2);
            int16_t pmin = 0, pmax = 0;
            size_t q = p + 7;
            if (id == 6) {
                pmin = r.s16(q);
                pmax = r.s16(q + 2);
                q += 24;
            }
            uint16_t tracks = r.u16(q);
            q += 2 + 1 + 4;
            for (uint16_t k = 0; k < tracks && !r.bad; ++k, q += 5) {
                Track t;
                t.wave = r.u16(q);
                t.wave_bank = r.u8(q + 2);
                t.loops = loops;
                t.pitch_min = pmin;
                t.pitch_max = pmax;
                cue->tracks.push_back(t);
            }
            p = q;
            break;
        }
        default:
            // Any other event stops the walk: its size is not known, and the
            // waves already found are the ones the cue plays.
            return true;
        }
    }
    return !r.bad;
}

// A sound: flags, category, volume, pitch, priority and length, then either
// one wave (a simple sound) or a list of clips; RPC and effect blocks sit
// between, each led by its own length.
bool read_sound(Reader &r, size_t at, Cue *cue) {
    uint8_t flags = r.u8(at);
    cue->category = r.u16(at + 1);
    cue->volume_db = volume_byte_db(r.u8(at + 3));
    cue->pitch = r.s16(at + 4);
    size_t p = at + 9;
    uint8_t clips = 0;
    if (flags & 1) {
        clips = r.u8(p);
        p += 1;
    } else {
        Track t;
        t.wave = r.u16(p);
        t.wave_bank = r.u8(p + 2);
        cue->tracks.push_back(t);
        p += 3;
    }
    if (flags & 0x0e) {
        uint16_t len = r.u16(p);
        uint8_t curves = r.u8(p + 2);
        for (uint8_t i = 0; i < curves; ++i)
            cue->rpc.push_back(r.u32(p + 3 + 4u * i));
        p += len;
    }
    if (flags & 0x10)
        p += r.u16(p);
    float clip_db = 0;
    for (uint8_t i = 0; i < clips && !r.bad; ++i, p += 9) {
        if (i == 0)
            clip_db = volume_byte_db(r.u8(p));
        if (!read_clip(r, r.u32(p + 1), cue))
            return false;
    }
    cue->volume_db += clip_db;
    return !r.bad;
}

// A variation table: a complex cue that picks one of several entries. The
// entry shape is in bits 3-5 of the table's flags.
bool read_variation(Reader &r, size_t at, Cue *cue) {
    uint16_t entries = r.u16(at);
    uint16_t type = (r.u16(at + 2) >> 3) & 7;
    size_t p = at + 8;
    for (uint16_t i = 0; i < entries && !r.bad; ++i) {
        Track t;
        switch (type) {
        case 0: // a wave, with weights
            t.wave = r.u16(p);
            t.wave_bank = r.u8(p + 2);
            cue->tracks.push_back(t);
            p += 5;
            break;
        case 1: // a sound, with byte weights
        case 3: // a sound, with float weights and flags
        {
            Cue sound;
            if (!read_sound(r, r.u32(p), &sound))
                return false;
            if (i == 0) {
                cue->category = sound.category;
                cue->volume_db = sound.volume_db;
                cue->pitch = sound.pitch;
                cue->rpc = sound.rpc;
            }
            cue->tracks.insert(cue->tracks.end(), sound.tracks.begin(), sound.tracks.end());
            p += type == 1 ? 6 : 16;
            break;
        }
        case 4: // a wave, unweighted
            t.wave = r.u16(p);
            t.wave_bank = r.u8(p + 2);
            cue->tracks.push_back(t);
            p += 3;
            break;
        default:
            return true;
        }
    }
    return !r.bad;
}

} // namespace

// SDBK, format 43.
bool parse_sound_bank(const uint8_t *data, size_t n, SoundBank *out, std::string *error) {
    Reader r{data, n};
    if (n < 0x8a || memcmp(data, "SDBK", 4))
        return fail(error, "not an XACT sound bank");
    uint16_t simple = r.u16(0x13), complex = r.u16(0x15);
    uint8_t wave_banks = r.u8(0x1b);
    uint16_t name_bytes = r.u16(0x1e);
    uint32_t simple_at = r.u32(0x22), complex_at = r.u32(0x26), names_at = r.u32(0x2a);
    uint32_t bank_names = r.u32(0x3a);
    SoundBank b;
    b.name = r.fixed(0x4a, 64);
    for (uint8_t i = 0; i < wave_banks; ++i)
        b.wave_banks.push_back(r.fixed(bank_names + 64u * i, 64));
    size_t count = (size_t)simple + complex;
    std::vector<std::string> names =
        names_at != NONE && name_bytes ? r.names(names_at, count) : std::vector<std::string>(count);
    for (uint16_t i = 0; i < simple && !r.bad; ++i) {
        Cue c;
        c.name = names[i];
        if (!read_sound(r, r.u32(simple_at + 5u * i + 1), &c))
            break;
        b.cues.push_back(std::move(c));
    }
    size_t p = complex_at;
    for (uint16_t i = 0; complex_at != NONE && i < complex && !r.bad; ++i, p += 15) {
        Cue c;
        c.name = names[simple + i];
        uint8_t flags = r.u8(p);
        bool ok = flags & 4 ? read_sound(r, r.u32(p + 1), &c) : read_variation(r, r.u32(p + 1), &c);
        if (!ok)
            break;
        b.cues.push_back(std::move(c));
    }
    if (r.bad || b.cues.size() != count)
        return fail(error, "XACT sound bank is truncated or malformed");
    *out = std::move(b);
    return true;
}

uint32_t Wave::samples_per_block() const {
    if (tag != TAG_ADPCM || !channels || block_align <= 7u * channels)
        return 0;
    return (block_align - 7u * channels) * 2 / channels + 2;
}

// WBND, version 44, header version 42: the signature, two versions and five
// segments (bank data, entry metadata, seek tables, entry names, wave data).
bool parse_wave_bank_segments(const uint8_t *data, size_t n, WaveBankHeader *out,
                              size_t *meta_needed, std::string *error) {
    Reader r{data, n};
    if (n < 52 || memcmp(data, "WBND", 4))
        return fail(error, "not an XACT wave bank");
    WaveBankHeader h;
    h.bank_offset = r.u32(12);
    h.bank_bytes = r.u32(16);
    h.meta_offset = r.u32(20);
    h.meta_bytes = r.u32(24);
    h.data_offset = r.u32(44);
    if (h.bank_bytes < 96)
        return fail(error, "XACT wave bank header is too short");
    size_t need = h.bank_offset + (size_t)h.bank_bytes;
    if (h.meta_offset + (size_t)h.meta_bytes > need)
        need = h.meta_offset + (size_t)h.meta_bytes;
    *meta_needed = need;
    *out = std::move(h);
    return true;
}

bool parse_wave_bank(const uint8_t *data, size_t n, WaveBankHeader *out, std::string *error) {
    size_t need = 0;
    WaveBankHeader h;
    if (!parse_wave_bank_segments(data, n, &h, &need, error))
        return false;
    Reader r{data, n};
    size_t b = h.bank_offset;
    h.flags = r.u32(b);
    uint32_t count = r.u32(b + 4);
    h.name = r.fixed(b + 8, 64);
    uint32_t entry_bytes = r.u32(b + 72);
    uint32_t alignment = r.u32(b + 80);
    uint32_t compact_format = r.u32(b + 84);
    if (r.bad)
        return fail(error, "XACT wave bank is truncated");
    bool compact = (h.flags & 0x20000) != 0;
    if (!compact && entry_bytes < 24)
        return fail(error, "XACT wave bank entries are too short");
    if (compact && entry_bytes < 4)
        return fail(error, "XACT wave bank compact entries are too short");
    if (count > h.meta_bytes / entry_bytes)
        return fail(error, "XACT wave bank has more entries than metadata");
    for (uint32_t i = 0; i < count; ++i) {
        size_t at = h.meta_offset + (size_t)entry_bytes * i;
        Wave w;
        uint32_t format = compact_format;
        if (compact) {
            uint32_t v = r.u32(at);
            w.offset = (v & 0x1fffff) * alignment;
            // The length is up to the next wave, or the end of the data.
            w.bytes = 0;
        } else {
            w.samples = r.u32(at) >> 4;
            format = r.u32(at + 4);
            w.offset = r.u32(at + 8);
            w.bytes = r.u32(at + 12);
            w.loop_start = r.u32(at + 16);
            w.loop_samples = r.u32(at + 20);
        }
        w.tag = format & 3;
        w.channels = (format >> 2) & 7;
        w.rate = (format >> 5) & 0x3ffff;
        uint32_t align = (format >> 23) & 0xff;
        w.bits = (format >> 31) ? 16 : 8;
        // ADPCM keeps the per-channel block size less the 22-byte offset the
        // format's header uses; PCM keeps a frame.
        w.block_align = w.tag == TAG_ADPCM ? (align + 22) * w.channels : align;
        if (w.tag == TAG_PCM && !w.block_align)
            w.block_align = w.channels * (w.bits / 8);
        h.waves.push_back(w);
    }
    if (compact) {
        for (size_t i = 0; i < h.waves.size(); ++i) {
            uint32_t next = i + 1 < h.waves.size() ? h.waves[i + 1].offset : 0;
            h.waves[i].bytes = next > h.waves[i].offset ? next - h.waves[i].offset : 0;
        }
    }
    if (r.bad)
        return fail(error, "XACT wave bank metadata is truncated");
    *out = std::move(h);
    return true;
}

namespace {
const int kCoef1[7] = {256, 512, 0, 192, 240, 460, 392};
const int kCoef2[7] = {0, -256, 0, 64, 0, -208, -232};
const int kAdapt[16] = {230, 230, 230, 230, 307, 409, 512, 614,
                        768, 614, 512, 409, 307, 230, 230, 230};

struct AdpcmState {
    int c1, c2, delta, s1, s2;
    int16_t step(int nibble) {
        int signed_nibble = nibble & 8 ? nibble - 16 : nibble;
        int predicted = (s1 * c1 + s2 * c2) / 256 + signed_nibble * delta;
        if (predicted > 32767)
            predicted = 32767;
        if (predicted < -32768)
            predicted = -32768;
        s2 = s1;
        s1 = predicted;
        delta = delta * kAdapt[nibble] / 256;
        if (delta < 16)
            delta = 16;
        return (int16_t)predicted;
    }
};
} // namespace

uint32_t decode_adpcm(const uint8_t *src, size_t bytes, uint32_t block_align, int channels,
                      std::vector<int16_t> *out) {
    if (channels < 1 || channels > 2 || block_align <= 7u * channels)
        return 0;
    uint32_t frames = 0;
    for (size_t at = 0; at + 7u * channels <= bytes; at += block_align) {
        size_t len = bytes - at < block_align ? bytes - at : block_align;
        const uint8_t *b = src + at;
        AdpcmState st[2];
        for (int ch = 0; ch < channels; ++ch) {
            int pred = b[ch];
            if (pred > 6)
                pred = 0;
            st[ch].c1 = kCoef1[pred];
            st[ch].c2 = kCoef2[pred];
            st[ch].delta = (int16_t)(b[channels + 2 * ch] | b[channels + 2 * ch + 1] << 8);
            st[ch].s1 = (int16_t)(b[3 * channels + 2 * ch] | b[3 * channels + 2 * ch + 1] << 8);
            st[ch].s2 = (int16_t)(b[5 * channels + 2 * ch] | b[5 * channels + 2 * ch + 1] << 8);
        }
        // The header's two samples come out oldest first.
        for (int ch = 0; ch < channels; ++ch)
            out->push_back((int16_t)st[ch].s2);
        for (int ch = 0; ch < channels; ++ch)
            out->push_back((int16_t)st[ch].s1);
        frames += 2;
        // Then the nibbles, high first; in stereo they alternate left, right.
        int ch = 0;
        for (size_t i = 7u * channels; i < len; ++i) {
            out->push_back(st[ch].step(b[i] >> 4));
            ch = (ch + 1) % channels;
            out->push_back(st[ch].step(b[i] & 15));
            ch = (ch + 1) % channels;
            frames += channels == 1 ? 2 : 1;
        }
    }
    return frames;
}

} // namespace xact
