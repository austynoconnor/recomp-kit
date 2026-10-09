// dls_synth.cpp - a DLS sampler for DirectMusic's software synth port.
//
// MGS2 (and any game like it) downloads its own instruments: a wave download
// (DMUS_DOWNLOADINFO_WAVE, type 2) per sample and an instrument download
// (type 1 or 3) per patch whose regions name those waves by download id. It
// then selects a patch on a performance channel (DMUS_PATCH_PMSG) and sends
// MIDI through SendPMsg: note-on, note-off, volume, pan, pitch bend.
//
// Each sounding note is one host audio channel playing the region's wave at
// the pitch the key asks for: rate = wave rate * 2^((key - unity note) / 12 +
// fine tune + bend). Velocity, channel volume, expression and the region's
// own attenuation give the channel volume; CC 10 gives the pan. A note-off
// stops a looping region (its loop would otherwise play for ever) and lets a
// one-shot play out, which is what a DLS envelope with a long release does
// to a sample that ends by itself. The loop is played from its loop start;
// the attack before it is skipped, and that is logged once.
//
// The waves stay in the guest memory the game filled until it unloads them.
#include "dls_synth.h"

#include "dx.h"
#include "host_api.h"
#include "../runtime/guest.h"
#include "../runtime/imports.h"
#include "../runtime/win32.h"

#include <math.h>
#include <stdio.h>
#include <algorithm>
#include <map>
#include <vector>

namespace {

// DMUS_DOWNLOADINFO types.
enum : uint32_t { DL_INSTRUMENT = 1, DL_WAVE = 2, DL_INSTRUMENT2 = 3 };
// DMUS_PMSGT types.
enum : uint32_t { PM_MIDI = 0, PM_NOTE = 1, PM_PATCH = 7 };

struct Wave {
    uint32_t pcm = 0, bytes = 0;
    uint32_t rate = 0;
    uint16_t channels = 0, bits = 0;
};

struct Region {
    uint16_t key_lo = 0, key_hi = 127, vel_lo = 0, vel_hi = 127;
    uint32_t wave = 0; // download id
    int unity = 60;
    int fine = 0;         // cents
    double gain_db = 0.0; // the region's attenuation, as gain
    bool looped = false;
    uint32_t loop_start = 0, loop_length = 0; // sample frames
};

struct Instrument {
    uint32_t dl = 0;
    std::vector<Region> regions;
};

struct Channel {
    uint32_t patch = 0;
    int volume = 100, expression = 127, pan = 64;
    int bend = 8192;
    double bend_range = 2.0; // semitones
};

struct Voice {
    int32_t host = -1;
    uint32_t pchannel = 0;
    int key = -1;
    int velocity = 0;
    double base_rate = 0.0;
    double gain_db = 0.0;
    uint32_t wave = 0;
    bool held = false;
    bool looped = false;
    uint64_t serial = 0;
};

std::map<uint32_t, Wave> g_waves;             // by download id
std::map<uint32_t, Instrument> g_instruments; // by patch
std::map<uint32_t, uint32_t> g_patch_of_dl;   // instrument download id -> patch
std::map<uint32_t, Channel> g_channels;       // by performance channel
std::vector<Voice> g_voices;
uint64_t g_serial = 0;
uint32_t g_notes = 0;

const size_t kMaxVoices = 32;

// A bounded view of one download buffer: reads past its end give zero.
struct Buf {
    uint32_t mem, size;
    uint32_t u32(uint32_t off) const {
        return off <= size && size - off >= 4 ? rd32(mem + off) : 0;
    }
    uint16_t u16(uint32_t off) const {
        return off <= size && size - off >= 2 ? rd16(mem + off) : 0;
    }
    bool has(uint32_t off, uint32_t n) const {
        return off <= size && size - off >= n;
    }
};

// The offset table after DMUS_DOWNLOADINFO: entry i's offset in the buffer.
uint32_t entry(const Buf &b, uint32_t i) {
    uint32_t n = b.u32(8);
    if (i >= n)
        return 0;
    return b.u32(16 + 4 * i);
}

bool parse_wave(uint32_t id, const Buf &b) {
    // DMUS_WAVE: ulFirstExtCkIdx, ulCopyrightIdx, ulWaveDataIdx, WAVEFORMATEX.
    uint32_t w = entry(b, 0);
    if (!w || !b.has(w, 12 + 16))
        return false;
    uint16_t tag = b.u16(w + 12);
    Wave v;
    v.channels = b.u16(w + 14);
    v.rate = b.u32(w + 16);
    v.bits = b.u16(w + 26);
    uint32_t data = entry(b, b.u32(w + 8));
    if (tag != 1 || !data || !v.rate || (v.bits != 8 && v.bits != 16) ||
        (v.channels != 1 && v.channels != 2))
        return false;
    // DMUS_WAVEDATA: cbSize, then the samples.
    uint32_t bytes = b.u32(data);
    if (!b.has(data + 4, bytes))
        bytes = b.size > data + 4 ? b.size - (data + 4) : 0;
    v.pcm = b.mem + data + 4;
    v.bytes = bytes;
    g_waves[id] = v;
    return true;
}

bool parse_instrument(uint32_t id, const Buf &b) {
    // DMUS_INSTRUMENT: ulPatch, ulFirstRegionIdx, ulGlobalArtIdx,
    // ulFirstExtCkIdx, ulCopyrightIdx, ulFlags.
    uint32_t ins = entry(b, 0);
    if (!ins || !b.has(ins, 24))
        return false;
    Instrument out;
    out.dl = id;
    uint32_t patch = b.u32(ins);
    uint32_t idx = b.u32(ins + 4);
    for (int guard = 0; idx && guard < 128; ++guard) {
        uint32_t r = entry(b, idx);
        // DMUS_REGION up to and including WSMPL's cSampleLoops.
        if (!r || !b.has(r, 56))
            break;
        Region g;
        g.key_lo = b.u16(r + 0);
        g.key_hi = b.u16(r + 2);
        g.vel_lo = b.u16(r + 4);
        g.vel_hi = b.u16(r + 6);
        if (g.vel_hi == 0)
            g.vel_hi = 127;
        g.wave = b.u32(r + 32); // WAVELINK.ulTableIndex
        g.unity = b.u16(r + 40);
        g.fine = (int16_t)b.u16(r + 42);
        g.gain_db = (double)(int32_t)b.u32(r + 44) / 655360.0;
        uint32_t loops = b.u32(r + 52);
        if (loops && b.has(r + 56, 16)) {
            g.looped = true;
            g.loop_start = b.u32(r + 64);
            g.loop_length = b.u32(r + 68);
        }
        out.regions.push_back(g);
        idx = b.u32(r + 16); // ulNextRegionIdx
    }
    if (out.regions.empty())
        return false;
    g_instruments[patch] = std::move(out);
    g_patch_of_dl[id] = patch;
    return true;
}

Channel &channel(uint32_t pchannel) {
    return g_channels[pchannel];
}

double db_of(double fraction) {
    return fraction > 0.0 ? 20.0 * log10(fraction) : -100.0;
}

// DirectSound volume, hundredths of a dB: the DLS velocity and controller
// curves are each (x / 127)^2.
int32_t volume_of(const Voice &v, const Channel &ch) {
    double db = 2.0 * db_of(v.velocity / 127.0) + 2.0 * db_of(ch.volume / 127.0) +
                2.0 * db_of(ch.expression / 127.0) + v.gain_db;
    double h = db * 100.0;
    return (int32_t)std::max(-10000.0, std::min(0.0, h));
}

int32_t pan_of(const Channel &ch) {
    double h = 0.0;
    if (ch.pan < 64)
        h = 100.0 * db_of(std::max(ch.pan, 1) / 64.0); // the right side quieter
    else if (ch.pan > 64)
        h = -100.0 * db_of((127 - ch.pan) / 63.0 + 1e-9); // the left side quieter
    return (int32_t)std::max(-10000.0, std::min(10000.0, h));
}

uint32_t rate_of(const Voice &v, const Channel &ch) {
    double semis = (ch.bend - 8192) / 8192.0 * ch.bend_range;
    double r = v.base_rate * pow(2.0, semis / 12.0);
    return (uint32_t)std::max(100.0, std::min(400000.0, r));
}

bool voice_sounding(const Voice &v) {
    return v.host >= 0 && host_audio_is_playing(v.host);
}

void stop_voice(Voice &v) {
    if (v.host >= 0)
        host_audio_stop(v.host);
    v.held = false;
    v.key = -1;
}

// A voice for a new note: the same key on the same channel first (it
// restarts), then one that has finished, then the oldest.
Voice *voice_for(uint32_t pchannel, int key) {
    for (Voice &v : g_voices)
        if (v.pchannel == pchannel && v.key == key) {
            stop_voice(v);
            return &v;
        }
    for (Voice &v : g_voices)
        if (!voice_sounding(v))
            return &v;
    if (g_voices.size() < kMaxVoices) {
        Voice v;
        v.host = dx_alloc_audio_channel();
        if (v.host < 0)
            return nullptr;
        g_voices.push_back(v);
        return &g_voices.back();
    }
    Voice *oldest = &g_voices[0];
    for (Voice &v : g_voices)
        if (v.serial < oldest->serial)
            oldest = &v;
    stop_voice(*oldest);
    return oldest;
}

const Instrument *instrument_for(uint32_t patch) {
    auto it = g_instruments.find(patch);
    if (it == g_instruments.end())
        it = g_instruments.find(patch & 0x7f); // the GM bank as a fallback
    return it == g_instruments.end() ? nullptr : &it->second;
}

void note_on(uint32_t pchannel, int key, int velocity) {
    Channel &ch = channel(pchannel);
    const Instrument *ins = instrument_for(ch.patch);
    if (!ins) {
        char k[48];
        snprintf(k, sizeof k, "dls.nopatch.%x", ch.patch);
        log_once(k, "dmusic: a note on patch 0x%x, which was never downloaded, is silent",
                 ch.patch);
        return;
    }
    const Region *rg = nullptr;
    for (const Region &r : ins->regions)
        if (key >= r.key_lo && key <= r.key_hi && velocity >= r.vel_lo && velocity <= r.vel_hi) {
            rg = &r;
            break;
        }
    if (!rg)
        return;
    auto w = g_waves.find(rg->wave);
    if (w == g_waves.end() || !w->second.bytes)
        return;
    const Wave &wave = w->second;
    Voice *v = voice_for(pchannel, key);
    if (!v)
        return;
    const uint32_t frame = (uint32_t)wave.channels * wave.bits / 8u;
    uint32_t pcm = wave.pcm, bytes = wave.bytes;
    bool loop = false;
    if (rg->looped && rg->loop_length) {
        uint32_t from = rg->loop_start * frame, len = rg->loop_length * frame;
        if (from < bytes && len <= bytes - from) {
            if (from)
                log_once("dls.loopattack", "dmusic: a looping DLS region plays from its loop "
                                           "start; the attack before the loop is skipped");
            pcm += from;
            bytes = len;
            loop = true;
        }
    }
    bytes -= bytes % frame;
    if (!bytes || !gm_valid(pcm, bytes))
        return;
    v->pchannel = pchannel;
    v->key = key;
    v->velocity = velocity;
    v->held = true;
    v->looped = loop;
    v->wave = rg->wave;
    v->gain_db = rg->gain_db;
    v->serial = ++g_serial;
    v->base_rate = wave.rate * pow(2.0, (key - rg->unity) / 12.0 + rg->fine / 1200.0);
    HostAudioPlay p{};
    p.channel = v->host;
    p.pcm = gm_ptr(pcm);
    p.bytes = bytes;
    p.sample_rate = (int32_t)wave.rate;
    p.channels = wave.channels;
    p.bits = wave.bits;
    p.loop = loop ? 1 : 0;
    p.volume = volume_of(*v, ch);
    p.pan = pan_of(ch);
    host_audio_play(&p);
    host_audio_set_frequency(v->host, rate_of(*v, ch));
    ++g_notes;
    log_once("dls.playing", "dmusic: playing DLS instruments the game downloaded");
}

void note_off(uint32_t pchannel, int key) {
    for (Voice &v : g_voices)
        if (v.pchannel == pchannel && v.key == key && v.held) {
            v.held = false;
            if (v.looped)
                stop_voice(v);
        }
}

void each_voice(uint32_t pchannel, void (*fn)(Voice &, const Channel &)) {
    const Channel &ch = channel(pchannel);
    for (Voice &v : g_voices)
        if (v.pchannel == pchannel && v.key >= 0)
            fn(v, ch);
}

void midi(uint32_t pchannel, uint8_t status, uint8_t a, uint8_t b) {
    Channel &ch = channel(pchannel);
    switch (status & 0xf0) {
    case 0x90:
        if (b)
            note_on(pchannel, a, b);
        else
            note_off(pchannel, a);
        break;
    case 0x80:
        note_off(pchannel, a);
        break;
    case 0xb0:
        if (a == 7 || a == 11) {
            (a == 7 ? ch.volume : ch.expression) = b;
            each_voice(pchannel, [](Voice &v, const Channel &c) {
                if (v.host >= 0)
                    host_audio_set_volume(v.host, volume_of(v, c));
            });
        } else if (a == 10) {
            ch.pan = b;
            each_voice(pchannel, [](Voice &v, const Channel &c) {
                if (v.host >= 0)
                    host_audio_set_pan(v.host, pan_of(c));
            });
        } else if (a == 120 || a == 123) { // all sound off, all notes off
            each_voice(pchannel, [](Voice &v, const Channel &) {
                if (v.looped || v.held)
                    stop_voice(v);
            });
        } else if (a == 121) { // reset all controllers
            ch.volume = 100;
            ch.expression = 127;
            ch.pan = 64;
            ch.bend = 8192;
        }
        break;
    case 0xc0:
        ch.patch = (ch.patch & 0xffff00u) | a;
        break;
    case 0xe0:
        ch.bend = a | (b << 7);
        each_voice(pchannel, [](Voice &v, const Channel &c) {
            if (v.host >= 0)
                host_audio_set_frequency(v.host, rate_of(v, c));
        });
        break;
    default:
        break;
    }
}

} // namespace

namespace dls {

bool download(uint32_t id, uint32_t mem, uint32_t size) {
    if (!mem || size < 16 || !gm_valid(mem, size))
        return false;
    Buf b{mem, size};
    uint32_t type = b.u32(0);
    unload(id);
    if (type == DL_WAVE)
        return parse_wave(id, b);
    if (type == DL_INSTRUMENT || type == DL_INSTRUMENT2)
        return parse_instrument(id, b);
    return false;
}

void unload(uint32_t id) {
    if (g_waves.erase(id))
        for (Voice &v : g_voices)
            if (v.key >= 0 && v.wave == id)
                stop_voice(v);
    auto p = g_patch_of_dl.find(id);
    if (p != g_patch_of_dl.end()) {
        auto ins = g_instruments.find(p->second);
        if (ins != g_instruments.end() && ins->second.dl == id)
            g_instruments.erase(ins);
        g_patch_of_dl.erase(p);
    }
}

void pmsg(uint32_t msg) {
    // DMUS_PMSG: dwSize 0, dwPChannel 0x18, dwType 0x28; the type's own
    // fields from 0x38.
    if (!msg || !gm_valid(msg, 0x40))
        return;
    uint32_t pchannel = rd32(msg + 0x18), type = rd32(msg + 0x28);
    if (type == PM_MIDI) {
        midi(pchannel, rd8(msg + 0x38), rd8(msg + 0x39), rd8(msg + 0x3a));
    } else if (type == PM_PATCH) {
        // DMUS_PATCH_PMSG: byInstrument, byMSB, byLSB.
        channel(pchannel).patch =
            ((uint32_t)rd8(msg + 0x39) << 16) | ((uint32_t)rd8(msg + 0x3a) << 8) | rd8(msg + 0x38);
    } else if (type == PM_NOTE && gm_valid(msg, 0x50)) {
        // DMUS_NOTE_PMSG: mtDuration 0x38, wMusicValue 0x3c, wMeasure 0x3e,
        // nOffset 0x40, bBeat, bGrid, bVelocity 0x44, bFlags 0x45,
        // bTimeRange, bMidiValue 0x47. Played as a note-on; the note-off a
        // performance would schedule from mtDuration is the game's to send.
        uint8_t flags = rd8(msg + 0x45), vel = rd8(msg + 0x44), key = rd8(msg + 0x47);
        if (flags & 1) // DMUS_NOTEF_NOTEON
            note_on(pchannel, key, vel);
        else
            note_off(pchannel, key);
        log_once("dls.note", "dmusic: note messages play their note-on; the note-off is "
                             "not scheduled from the duration");
    }
}

void reset() {
    g_waves.clear();
    g_instruments.clear();
    g_patch_of_dl.clear();
    g_channels.clear();
    for (Voice &v : g_voices)
        if (v.host >= 0)
            dx_free_audio_channel(v.host);
    g_voices.clear();
    g_serial = 0;
    g_notes = 0;
}

uint32_t notes_started() {
    return g_notes;
}
uint32_t instruments() {
    return (uint32_t)g_instruments.size();
}
uint32_t waves() {
    return (uint32_t)g_waves.size();
}

} // namespace dls
