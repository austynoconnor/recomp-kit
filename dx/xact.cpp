// xact.cpp - an XACT 2 engine (DirectX SDK, August 2007) on the host mixer.
//
// A game creates the engine with CoCreateInstance(CLSID_XACTEngine), hands
// it its global settings and then loads sound banks and wave banks and plays
// cues by index. This engine reads all three formats (xact_banks.h): the
// settings name the categories and variables and hold the RPC curves, a sound
// bank says which wave each cue plays, and a wave bank holds the waves, in
// guest memory or, for a streaming bank, in a file the game opened.
//
// A cue plays one of its tracks on its own host mixer channel. In-memory
// waves are decoded whole when they start; streamed waves are decoded half a
// second at a time and appended behind what is playing, from the frame pump
// and from DoWork, so a three-minute music track never sits decoded in
// memory. MS ADPCM and 8/16-bit PCM are decoded; XMA and WMA are not (a cue
// that needs one plays silently and still runs its course).
//
// A cue is PREPARED after Prepare, PLAYING once played and STOPPED when its
// wave has run its length on the guest clock or it is stopped. The clock and
// not the mixer decides, so a host without audio (the smoke) still sees each
// cue last as long as its sound does. Volume is the sound's authored volume,
// its category's (authored and SetVolume), the cue's RPC curves over its
// variables and its SetMatrixCoefficients gain; pitch is the sound's own, a
// random pick in its variation range and the RPC pitch.
//
// Notifications go to the callback the game passed to Initialize, for the
// types it registered: CUEDESTROYED synchronously from Destroy (Bully's only
// registration), the others from the next DoWork or frame.
//
// The vtable orders and pop counts follow xact.h of the August 2007 SDK
// (XACT 2.9). Bully: Scholarship Edition's own calls confirm the engine's:
// GetRendererDetails at +0x10 with three dwords, Initialize at +0x18 and
// RegisterNotification at +0x3c. Only IXACTEngine is a COM interface; the
// banks, cues and waves are plain interfaces without IUnknown, released with
// Destroy.
//
// RECOMP_TRACE_XACT logs the first calls of each method and what each bank
// held when it loaded.
#include "com.h"
#include "dx.h"
#include "host_api.h"
#include "xact_banks.h"
#include "../platform/os.h"
#include "../runtime/guest.h"
#include "../runtime/memory.h"
#include "../runtime/win32.h"

#include <iterator>
#include <map>
#include <math.h>
#include <memory>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

namespace {

// {962f5027-99be-4692-a468-85802cf8de61}, its debug and auditioning
// variants, and IID_IXACTEngine {e72c1b9a-d717-41c0-81a6-50eb56e80649}.
const uint8_t CLSID_XACTEngine_[16] = {0x27, 0x50, 0x2f, 0x96, 0xbe, 0x99, 0x92, 0x46,
                                       0xa4, 0x68, 0x85, 0x80, 0x2c, 0xf8, 0xde, 0x61};
const uint8_t CLSID_XACTDebugEngine_[16] = {0x1d, 0x2b, 0x6a, 0x02, 0x04, 0xf2, 0x4e, 0x48,
                                            0xab, 0x36, 0xab, 0x2b, 0x66, 0x8f, 0x95, 0x4e};
const uint8_t IID_IXACTEngine_[16] = {0x9a, 0x1b, 0x2c, 0xe7, 0x17, 0xd7, 0xc0, 0x41,
                                      0x81, 0xa6, 0x50, 0xeb, 0x56, 0xe8, 0x06, 0x49};

const uint32_t XACT_STATE_PREPARED = 0x04;
const uint32_t XACT_STATE_PLAYING = 0x08;
const uint32_t XACT_STATE_STOPPED = 0x20;
const uint32_t XACT_STATE_PAUSED = 0x40;
const uint16_t XACTINDEX_INVALID = 0xffff;

// XACTNOTIFICATIONTYPE_*.
enum : uint8_t {
    N_CUEPREPARED = 1,
    N_CUEPLAY = 2,
    N_CUESTOP = 3,
    N_CUEDESTROYED = 4,
    N_SOUNDBANKDESTROYED = 6,
    N_WAVEBANKDESTROYED = 7,
    N_WAVEBANKPREPARED = 17,
};
const uint8_t XACT_FLAG_NOTIFICATION_PERSIST = 0x01;

bool tracing() {
    static const bool on = recomp_env("TRACE_XACT") != nullptr;
    return on;
}

// The first calls of each method with their raw arguments.
void xtrace(const char *name, X86 *c, int n) {
    if (!tracing())
        return;
    static std::map<std::string, int> seen;
    if (++seen[name] > 24)
        return;
    char line[512];
    int at = snprintf(line, sizeof line, "xact: %s", name);
    for (int i = 0; i < n && at < (int)sizeof line - 12; ++i)
        at += snprintf(line + at, sizeof line - at, " %08x", arg(c, i));
    LOGW("%s", line);
}
#define XT(n) xtrace(__func__, c, n)

float argf(X86 *c, int i) {
    uint32_t v = arg(c, i);
    float f;
    memcpy(&f, &v, 4);
    return f;
}
void put(uint32_t p, uint32_t v) {
    if (p && gm_valid(p, 4))
        wr32(p, v);
}
void zero_bytes(uint32_t p, uint32_t n) {
    if (p && gm_valid(p, n))
        memset(gm_ptr(p), 0, n);
}
void ok(X86 *c) {
    set_eax(c, S_OK);
}

// --- State -------------------------------------------------------------
struct WaveBankData {
    xact::WaveBankHeader h;
    bool streaming = false;
    uint32_t mem = 0; // in memory: the guest address of the bank
    int fd = -1;      // streaming: our own descriptor on the game's file
    int64_t base = 0; // streaming: the bank's offset in that file
    uint32_t view = 0;
    ~WaveBankData() {
        if (fd >= 0)
            os_fd_close(fd);
    }
    // `bytes` of wave data at `offset` from the start of the bank's data.
    bool read(uint32_t offset, uint32_t bytes, std::vector<uint8_t> *out) const {
        out->resize(bytes);
        if (!bytes)
            return true;
        if (!streaming) {
            uint32_t at = mem + h.data_offset + offset;
            if (!gm_valid(at, bytes))
                return false;
            memcpy(out->data(), gm_ptr(at), bytes);
            return true;
        }
        if (os_fd_seek(fd, base + h.data_offset + offset, OS_SEEK_SET) < 0)
            return false;
        int64_t got = os_fd_read(fd, out->data(), bytes);
        if (got < 0)
            return false;
        out->resize((size_t)got);
        return true;
    }
};

struct SoundBankData {
    xact::SoundBank sb;
    bool parsed = false;
    uint32_t view = 0;
};

// One wave playing on one host channel.
struct Voice {
    std::shared_ptr<WaveBankData> bank;
    xact::Wave wave;
    int32_t channel = -1;
    uint32_t loops = 0;     // still to play after this pass; 255 for ever
    uint32_t next = 0;      // streaming: the next byte of the wave to decode
    bool feeding = false;   // streaming: more to append
    uint32_t start_ms = 0;  // on the guest clock
    uint32_t length_ms = 0; // 0 for a loop that never ends
    uint32_t paused_at = 0;
    float pitch_cents = 0; // the play's own: sound, variation
    bool active = false;
};

// A cue or a wave: the object the game holds, and what it is playing.
struct Instance {
    uint32_t sound_bank = 0; // ComObj id
    uint16_t cue = XACTINDEX_INVALID;
    uint32_t state = XACT_STATE_PREPARED;
    bool paused = false;
    std::map<uint16_t, float> vars;
    float matrix_db = 0, pan = 0;      // pan in hundredths of a dB, DirectSound's
    float wave_db = 0, wave_cents = 0; // IXACTWave SetVolume / SetPitch
    bool is_wave = false;
    // IXACTWave: the wave it plays directly.
    std::shared_ptr<WaveBankData> wave_bank;
    uint16_t wave_index = 0;
    uint8_t wave_loops = 0;
    Voice v;
};

struct Registration {
    uint8_t type, flags;
    uint32_t sound_bank, wave_bank, cue;
    uint16_t cue_index;
    uint32_t context;
};
struct Pending {
    uint8_t type;
    uint32_t context;
    uint8_t body[24];
    uint32_t body_bytes;
};

struct Engine {
    xact::Settings settings;
    bool settings_parsed = false;
    uint32_t callback = 0;
    std::map<uint32_t, std::shared_ptr<WaveBankData>> wave_banks; // by object id
    std::map<uint32_t, std::unique_ptr<SoundBankData>> sound_banks;
    std::map<uint32_t, std::unique_ptr<Instance>> instances;
    std::vector<std::unique_ptr<Instance>> orphans; // fire-and-forget plays
    std::map<uint16_t, float> category_gain;        // SetVolume, linear
    std::map<uint16_t, bool> category_paused;
    std::map<uint16_t, float> globals;
    std::vector<Registration> regs;
    std::vector<Pending> pending;
    uint32_t note_buf = 0;
    uint32_t rng = 0x2545f491;
    // Counters for the trace and the tests.
    uint32_t plays = 0, streamed = 0, missing = 0, undecodable = 0;
};
Engine &E() {
    static Engine e;
    return e;
}

uint32_t rnd() {
    uint32_t &x = E().rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

uint32_t obj_id(uint32_t view) {
    ComObj *o = view ? com_this(view) : nullptr;
    return o ? o->id : 0;
}

// --- Notifications -----------------------------------------------------
// XACT_NOTIFICATION, packed: type, timestamp, context, then the union.
void deliver(X86 *c, const Pending &p) {
    Engine &e = E();
    if (!e.callback)
        return;
    if (!e.note_buf)
        e.note_buf = heap_alloc(64, true);
    if (!e.note_buf)
        return;
    zero_bytes(e.note_buf, 64);
    wr8(e.note_buf, p.type);
    wr32(e.note_buf + 1, host_millis());
    wr32(e.note_buf + 5, p.context);
    memcpy(gm_ptr(e.note_buf + 9), p.body, p.body_bytes);
    guest_call(c, e.callback, e.note_buf);
}

// Queues (or, with `c`, delivers now) every registration this event matches.
void notify(X86 *c, uint8_t type, uint32_t sb_view, uint32_t wb_view, uint32_t cue_view,
            uint16_t cue_index) {
    Engine &e = E();
    if (!e.callback)
        return;
    for (size_t i = 0; i < e.regs.size();) {
        Registration r = e.regs[i];
        bool match =
            r.type == type && (!r.cue || r.cue == cue_view) &&
            (!r.sound_bank || r.sound_bank == sb_view) &&
            (!r.wave_bank || r.wave_bank == wb_view) &&
            (r.cue_index == XACTINDEX_INVALID || !r.sound_bank || r.cue_index == cue_index);
        if (!match) {
            ++i;
            continue;
        }
        Pending p{type, r.context, {}, 0};
        if (type == N_WAVEBANKPREPARED || type == N_WAVEBANKDESTROYED) {
            memcpy(p.body, &wb_view, 4);
            p.body_bytes = 4;
        } else if (type == N_SOUNDBANKDESTROYED) {
            memcpy(p.body, &sb_view, 4);
            p.body_bytes = 4;
        } else { // XACT_NOTIFICATION_CUE: index, sound bank, cue
            memcpy(p.body, &cue_index, 2);
            memcpy(p.body + 2, &sb_view, 4);
            memcpy(p.body + 6, &cue_view, 4);
            p.body_bytes = 10;
        }
        if (!(r.flags & XACT_FLAG_NOTIFICATION_PERSIST))
            e.regs.erase(e.regs.begin() + (long)i);
        else
            ++i;
        if (c)
            deliver(c, p);
        else
            e.pending.push_back(p);
    }
}

void flush_notifications(X86 *c) {
    Engine &e = E();
    while (!e.pending.empty()) {
        Pending p = e.pending.front();
        e.pending.erase(e.pending.begin());
        deliver(c, p);
    }
}

// --- Voices ------------------------------------------------------------
const xact::Cue *cue_of(const Instance &in) {
    auto it = E().sound_banks.find(in.sound_bank);
    if (it == E().sound_banks.end() || in.cue >= it->second->sb.cues.size())
        return nullptr;
    return &it->second->sb.cues[in.cue];
}

float var_value(const Instance &in, uint16_t index) {
    auto it = in.vars.find(index);
    if (it != in.vars.end())
        return it->second;
    auto g = E().globals.find(index);
    if (g != E().globals.end())
        return g->second;
    const auto &vars = E().settings.variables;
    return index < vars.size() ? vars[index].initial : 0;
}

// The category's gain in decibels, its parents' included.
float category_db(uint16_t cat) {
    float db = 0;
    const auto &cats = E().settings.categories;
    for (int depth = 0; cat < cats.size() && depth < 8; ++depth) {
        db += cats[cat].volume_db;
        auto g = E().category_gain.find(cat);
        if (g != E().category_gain.end())
            db += g->second > 0 ? 20 * log10f(g->second) : -100;
        cat = cats[cat].parent;
    }
    return db;
}

bool category_paused(uint16_t cat) {
    const auto &cats = E().settings.categories;
    for (int depth = 0; depth < 8; ++depth) {
        auto p = E().category_paused.find(cat);
        if (p != E().category_paused.end() && p->second)
            return true;
        if (cat >= cats.size())
            break;
        cat = cats[cat].parent;
    }
    return false;
}

// Volume in hundredths of a dB and pitch in cents, from everything but the
// play's own pitch.
void mix_of(const Instance &in, float *volume, float *cents) {
    float db = in.matrix_db + in.wave_db, pc = in.wave_cents;
    if (const xact::Cue *cue = in.is_wave ? nullptr : cue_of(in)) {
        db += cue->volume_db + category_db(cue->category);
        for (uint32_t code : cue->rpc) {
            const xact::RpcCurve *k = E().settings.curve(code);
            if (!k)
                continue;
            float y = k->eval(var_value(in, k->variable));
            if (k->parameter == 0)
                db += y / 100;
            else if (k->parameter == 1)
                pc += y;
        }
        if (category_paused(cue->category))
            db = -100;
    }
    if (in.paused)
        db = -100;
    *volume = db * 100;
    *cents = pc;
}

int32_t clamp_volume(float v) {
    return v > 0 ? 0 : v < -10000 ? -10000 : (int32_t)v;
}

uint32_t pitched_rate(uint32_t rate, float cents) {
    if (cents > 2400)
        cents = 2400;
    if (cents < -2400)
        cents = -2400;
    return (uint32_t)(rate * powf(2.0f, cents / 1200));
}

void apply_mix(Instance &in) {
    Voice &v = in.v;
    if (!v.active || v.channel < 0)
        return;
    float vol, cents;
    mix_of(in, &vol, &cents);
    host_audio_set_volume(v.channel, clamp_volume(vol));
    host_audio_set_pan(v.channel, (int32_t)in.pan);
    host_audio_set_frequency(v.channel, pitched_rate(v.wave.rate, cents + v.pitch_cents));
}

// Decodes `bytes` of the wave from byte `at` to 16-bit PCM.
bool decode(const Voice &v, uint32_t at, uint32_t bytes, std::vector<int16_t> *pcm) {
    std::vector<uint8_t> raw;
    if (!v.bank->read(v.wave.offset + at, bytes, &raw))
        return false;
    if (v.wave.tag == xact::TAG_ADPCM) {
        xact::decode_adpcm(raw.data(), raw.size(), v.wave.block_align, v.wave.channels, pcm);
        return true;
    }
    if (v.wave.bits == 8) {
        for (uint8_t b : raw)
            pcm->push_back((int16_t)((b - 128) * 256));
    } else {
        size_t n = raw.size() / 2;
        size_t old = pcm->size();
        pcm->resize(old + n);
        memcpy(pcm->data() + old, raw.data(), n * 2);
    }
    return true;
}

uint32_t block_bytes(const xact::Wave &w) {
    return w.block_align ? w.block_align : 2u * w.channels;
}
uint32_t block_samples(const xact::Wave &w) {
    uint32_t spb = w.tag == xact::TAG_ADPCM ? w.samples_per_block() : 1;
    return spb ? spb : 1;
}

// The bytes of wave data that make `seconds`, in whole blocks.
uint32_t chunk_bytes(const xact::Wave &w, float seconds) {
    uint32_t blocks = (uint32_t)(w.rate * seconds / block_samples(w)) + 1;
    return blocks * block_bytes(w);
}

uint32_t loop_start_bytes(const xact::Wave &w) {
    if (w.loop_start >= w.samples)
        return 0;
    return w.loop_start / block_samples(w) * block_bytes(w);
}

// Appends more of a streamed wave while the voice has less than a second
// ahead of it.
void refill(Voice &v) {
    if (!v.active || !v.feeding || v.channel < 0)
        return;
    uint32_t frame = 2u * v.wave.channels;
    for (int guard = 0; guard < 4; ++guard) {
        if (host_audio_voice_remaining_bytes(v.channel) >= v.wave.rate * frame)
            return;
        if (v.next >= v.wave.bytes) {
            if (!v.loops) {
                v.feeding = false;
                return;
            }
            if (v.loops != 255)
                --v.loops;
            v.next = loop_start_bytes(v.wave);
        }
        uint32_t n = chunk_bytes(v.wave, 0.5f);
        if (n > v.wave.bytes - v.next)
            n = v.wave.bytes - v.next;
        std::vector<int16_t> pcm;
        if (!decode(v, v.next, n, &pcm) || pcm.empty()) {
            v.feeding = false;
            return;
        }
        v.next += n;
        if (host_audio_queue(v.channel, pcm.data(), (uint32_t)(pcm.size() * 2)) <= 0) {
            v.feeding = false; // a host without streaming: what started plays
            return;
        }
    }
}

void stop_voice(Voice &v) {
    if (v.channel >= 0) {
        host_audio_stop(v.channel);
        dx_free_audio_channel(v.channel);
    }
    v.channel = -1;
    v.active = false;
    v.feeding = false;
    v.bank.reset();
}

std::shared_ptr<WaveBankData> find_wave_bank(const std::string &name) {
    std::shared_ptr<WaveBankData> best;
    uint32_t best_id = 0;
    for (auto &kv : E().wave_banks)
        if (kv.second->h.name == name && kv.first >= best_id) {
            best = kv.second;
            best_id = kv.first;
        }
    return best;
}

// RECOMP_XACT_DUMP=<dir>: the first 16 waves played, decoded, as WAV files
// named for their bank and index; a streamed one up to 20 seconds.
void dump_wave(const Voice &v, uint16_t index) {
    static const char *dir = recomp_env("XACT_DUMP");
    static int count = 0;
    if (!dir || !*dir || count >= 16)
        return;
    ++count;
    std::vector<int16_t> pcm;
    uint32_t n = v.bank->streaming ? chunk_bytes(v.wave, 20.0f) : v.wave.bytes;
    if (n > v.wave.bytes)
        n = v.wave.bytes;
    if (!decode(v, 0, n, &pcm))
        return;
    char path[1024];
    snprintf(path, sizeof path, "%s/%02d_%s_%u.wav", dir, count, v.bank->h.name.c_str(), index);
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    uint32_t bytes = (uint32_t)pcm.size() * 2, ch = v.wave.channels, rate = v.wave.rate;
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    uint32_t vals[] = {36 + bytes, 0, 0, 16, 0, rate, rate * ch * 2, 0, 0};
    memcpy(h + 4, &vals[0], 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    memcpy(h + 16, &vals[3], 4);
    uint16_t fmt[2] = {1, (uint16_t)ch};
    memcpy(h + 20, fmt, 4);
    memcpy(h + 24, &vals[5], 4);
    memcpy(h + 28, &vals[6], 4);
    uint16_t ba[2] = {(uint16_t)(ch * 2), 16};
    memcpy(h + 32, ba, 4);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &bytes, 4);
    fwrite(h, 1, 44, f);
    fwrite(pcm.data(), 1, bytes, f);
    fclose(f);
}

// Starts the instance's wave. False when there is nothing it can play, which
// leaves it to end as STOPPED.
bool start(Instance &in) {
    Engine &e = E();
    Voice &v = in.v;
    stop_voice(v);
    v.pitch_cents = 0;
    std::shared_ptr<WaveBankData> bank;
    uint16_t index = 0;
    uint8_t loops = 0;
    if (in.is_wave) {
        bank = in.wave_bank;
        index = in.wave_index;
        loops = in.wave_loops;
    } else {
        const xact::Cue *cue = cue_of(in);
        if (!cue || cue->tracks.empty())
            return false;
        const xact::Track &t = cue->tracks[rnd() % cue->tracks.size()];
        const auto &names = e.sound_banks[in.sound_bank]->sb.wave_banks;
        uint8_t wb = t.wave_bank < names.size() ? t.wave_bank : 0;
        bank = names.empty() ? nullptr : find_wave_bank(names[wb]);
        index = t.wave;
        loops = t.loops;
        v.pitch_cents = cue->pitch;
        if (t.pitch_max > t.pitch_min)
            v.pitch_cents +=
                t.pitch_min + (float)(rnd() % (uint32_t)(t.pitch_max - t.pitch_min + 1));
        else
            v.pitch_cents += t.pitch_min;
        if (!bank && tracing() && e.missing < 40)
            LOGW("xact: cue '%s' needs wave bank '%s', which is not loaded", cue->name.c_str(),
                 names.empty() ? "" : names[wb].c_str());
    }
    if (!bank || index >= bank->h.waves.size()) {
        ++e.missing;
        return false;
    }
    const xact::Wave &w = bank->h.waves[index];
    if ((w.tag != xact::TAG_ADPCM && w.tag != xact::TAG_PCM) || !w.rate || !w.channels ||
        w.channels > 2) {
        if (tracing() && e.undecodable < 20)
            LOGW("xact: wave %u of '%s' is format %u, which this engine does not decode", index,
                 bank->h.name.c_str(), w.tag);
        ++e.undecodable;
        return false;
    }
    v.bank = bank;
    v.wave = w;
    v.loops = loops;
    v.start_ms = host_millis();
    dump_wave(v, index);
    float vol, cents;
    mix_of(in, &vol, &cents);
    uint64_t samples = (uint64_t)(w.bytes / block_bytes(w)) * block_samples(w);
    if (w.samples && w.samples < samples)
        samples = w.samples;
    uint32_t rate = pitched_rate(w.rate, cents + v.pitch_cents);
    v.length_ms =
        loops == 255 ? 0 : (uint32_t)(samples * (loops + 1u) * 1000.0 / (rate ? rate : 1)) + 1;

    std::vector<int16_t> pcm;
    bool stream = bank->streaming;
    uint32_t first = stream ? chunk_bytes(w, 1.5f) : w.bytes;
    if (first > w.bytes)
        first = w.bytes;
    if (!decode(v, 0, first, &pcm) || pcm.empty()) {
        v.bank.reset();
        return false;
    }
    v.next = first;
    bool loop_whole = false;
    if (!stream) {
        if (loops == 255) {
            loop_whole = true;
        } else {
            size_t one = pcm.size();
            for (uint32_t k = 0; k < loops; ++k)
                pcm.insert(pcm.end(), pcm.begin(), pcm.begin() + (long)one);
        }
    }
    v.channel = dx_alloc_audio_channel();
    if (v.channel < 0) {
        v.bank.reset();
        return false;
    }
    HostAudioPlay p{};
    p.channel = v.channel;
    p.pcm = pcm.data();
    p.bytes = (uint32_t)(pcm.size() * 2);
    p.sample_rate = (int32_t)w.rate;
    p.channels = w.channels;
    p.bits = 16;
    p.loop = loop_whole ? 1 : 0;
    p.volume = clamp_volume(vol);
    p.pan = (int32_t)in.pan;
    host_audio_play(&p);
    v.active = true;
    v.feeding = stream && (first < w.bytes || loops);
    if (rate != w.rate)
        host_audio_set_frequency(v.channel, rate);
    ++e.plays;
    if (stream)
        ++e.streamed;
    refill(v);
    return true;
}

// Moves a playing instance to STOPPED once its sound has run its length.
void update(Instance &in) {
    if (in.state != XACT_STATE_PLAYING || in.paused)
        return;
    Voice &v = in.v;
    refill(v);
    if (v.active && (!v.length_ms || host_millis() - v.start_ms < v.length_ms))
        return;
    stop_voice(v);
    in.state = XACT_STATE_STOPPED;
}

void play(Instance &in) {
    in.state = XACT_STATE_PLAYING;
    in.paused = false;
    if (!start(in))
        in.state = XACT_STATE_STOPPED;
}

void stop(Instance &in) {
    stop_voice(in.v);
    in.state = XACT_STATE_STOPPED;
}

void pause(Instance &in, bool on) {
    if (on == in.paused)
        return;
    in.paused = on;
    Voice &v = in.v;
    if (on)
        v.paused_at = host_millis();
    else
        v.start_ms += host_millis() - v.paused_at; // the pause does not count
    apply_mix(in);
}

// Every live instance, the fire-and-forget ones included.
template <class F> void each_instance(F f) {
    for (auto &kv : E().instances)
        f(*kv.second);
    for (auto &o : E().orphans)
        f(*o);
}

// --- Objects -----------------------------------------------------------
ComObj *create_engine() {
    return com_new(K_XACT);
}

// Hands out a new object through `out` as `iface`. Null when it cannot.
ComObj *make(X86 *c, ComIface iface, uint32_t out) {
    if (!out || !gm_valid(out, 4)) {
        set_eax(c, E_INVALIDARG);
        return nullptr;
    }
    ComObj *o = com_new(K_XACT);
    uint32_t view = o ? com_view(o, iface) : 0;
    wr32(out, view);
    set_eax(c, view ? S_OK : E_OUTOFMEMORY);
    return view ? o : nullptr;
}

// Everything this engine keeps for an object goes with it.
void on_destroy(ComObj *o) {
    Engine &e = E();
    auto in = e.instances.find(o->id);
    if (in != e.instances.end()) {
        stop_voice(in->second->v);
        e.instances.erase(in);
    }
    e.wave_banks.erase(o->id);
    if (e.sound_banks.erase(o->id)) {
        // Its cues stop with it, as XACT's do.
        each_instance([&](Instance &i) {
            if (i.sound_bank == o->id) {
                stop(i);
                i.sound_bank = 0;
            }
        });
    }
}

void pump(X86 *c) {
    Engine &e = E();
    for (auto &kv : e.instances)
        update(*kv.second);
    for (size_t i = 0; i < e.orphans.size();) {
        update(*e.orphans[i]);
        if (e.orphans[i]->state == XACT_STATE_STOPPED)
            e.orphans.erase(e.orphans.begin() + (long)i);
        else
            ++i;
    }
    if (c)
        flush_notifications(c);
    static uint32_t last_report = 0;
    if (tracing() && host_millis() - last_report >= 10000) {
        last_report = host_millis();
        size_t playing = 0;
        each_instance([&](Instance &i) { playing += i.state == XACT_STATE_PLAYING; });
        LOGW("xact: %u plays (%u streamed), %zu playing, %u without a wave bank, %u undecodable",
             e.plays, e.streamed, playing, e.missing, e.undecodable);
    }
}

Instance *inst(X86 *c) {
    ComObj *o = com_this_arg(c);
    if (!o)
        return nullptr;
    auto it = E().instances.find(o->id);
    return it == E().instances.end() ? nullptr : it->second.get();
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
    const char *id = "{host}", *name = "Host mixer";
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
// XACT_RUNTIME_PARAMETERS, packed: look-ahead, the settings buffer and its
// size, flags, allocation attributes, two file callbacks, the notification
// callback and the renderer id.
void E_Initialize(X86 *c) {
    XT(2);
    Engine &e = E();
    uint32_t p = arg(c, 1);
    if (p && gm_valid(p, 36)) {
        uint32_t buf = rd32(p + 4), size = rd32(p + 8);
        e.callback = rd32(p + 28);
        std::string why;
        if (buf && size && gm_valid(buf, size)) {
            e.settings_parsed = xact::parse_settings(gm_ptr(buf), size, &e.settings, &why);
            if (!e.settings_parsed)
                LOGW("xact: global settings not read: %s", why.c_str());
            else if (tracing())
                LOGW("xact: settings: %zu categories, %zu variables, %zu RPC curves",
                     e.settings.categories.size(), e.settings.variables.size(),
                     e.settings.curves.size());
        }
    }
    set_eax(c, S_OK);
}
void E_ShutDown(X86 *c) {
    each_instance([](Instance &i) { stop(i); });
    E().orphans.clear();
    set_eax(c, S_OK);
}
void E_DoWork(X86 *c) {
    pump(c);
    set_eax(c, S_OK);
}
void E_CreateSoundBank(X86 *c) {
    XT(6);
    uint32_t buf = arg(c, 1), size = arg(c, 2);
    ComObj *o = make(c, IF_XACT_SOUNDBANK, arg(c, 5));
    if (!o)
        return;
    auto sb = std::make_unique<SoundBankData>();
    sb->view = rd32(arg(c, 5));
    std::string why;
    if (buf && size && gm_valid(buf, size))
        sb->parsed = xact::parse_sound_bank(gm_ptr(buf), size, &sb->sb, &why);
    else
        why = "no buffer";
    if (!sb->parsed)
        LOGW("xact: sound bank not read: %s", why.c_str());
    else if (tracing())
        LOGW("xact: sound bank '%s': %zu cues, wave bank '%s'", sb->sb.name.c_str(),
             sb->sb.cues.size(), sb->sb.wave_banks.empty() ? "" : sb->sb.wave_banks[0].c_str());
    E().sound_banks[o->id] = std::move(sb);
}
void add_wave_bank(ComObj *o, std::shared_ptr<WaveBankData> wb) {
    if (tracing())
        LOGW("xact: %s wave bank '%s': %zu waves", wb->streaming ? "streaming" : "in-memory",
             wb->h.name.c_str(), wb->h.waves.size());
    E().wave_banks[o->id] = std::move(wb);
}
void E_CreateInMemoryWaveBank(X86 *c) {
    XT(6);
    uint32_t buf = arg(c, 1), size = arg(c, 2);
    ComObj *o = make(c, IF_XACT_WAVEBANK, arg(c, 5));
    if (!o)
        return;
    auto wb = std::make_shared<WaveBankData>();
    wb->view = rd32(arg(c, 5));
    wb->mem = buf;
    std::string why;
    if (!buf || !size || !gm_valid(buf, size) ||
        !xact::parse_wave_bank(gm_ptr(buf), size, &wb->h, &why)) {
        LOGW("xact: in-memory wave bank not read: %s", why.empty() ? "no buffer" : why.c_str());
        return;
    }
    add_wave_bank(o, std::move(wb));
}
// XACT_WAVEBANK_STREAMING_PARAMETERS: the file handle, the bank's offset in
// it, flags and the packet size. The bank is read through a descriptor of
// our own on the same file, so the game's own file position is never moved.
void E_CreateStreamingWaveBank(X86 *c) {
    XT(3);
    uint32_t p = arg(c, 1);
    ComObj *o = make(c, IF_XACT_WAVEBANK, arg(c, 2));
    if (!o)
        return;
    uint32_t view = rd32(arg(c, 2));
    std::string path, why;
    auto wb = std::make_shared<WaveBankData>();
    wb->view = view;
    wb->streaming = true;
    if (!p || !gm_valid(p, 12) || !win32_file_handle_position(rd32(p), &path, nullptr)) {
        LOGW("xact: streaming wave bank has no file");
        return;
    }
    wb->base = rd32(p + 4);
    wb->fd = os_fd_open(path.c_str(), OS_O_RDONLY);
    std::vector<uint8_t> head(52);
    size_t need = 0;
    bool ok = wb->fd >= 0 && os_fd_seek(wb->fd, wb->base, OS_SEEK_SET) >= 0 &&
              os_fd_read(wb->fd, head.data(), head.size()) == (int64_t)head.size() &&
              xact::parse_wave_bank_segments(head.data(), head.size(), &wb->h, &need, &why);
    if (ok && need < (64u << 20)) {
        head.resize(need);
        ok = os_fd_seek(wb->fd, wb->base, OS_SEEK_SET) >= 0 &&
             os_fd_read(wb->fd, head.data(), need) == (int64_t)need &&
             xact::parse_wave_bank(head.data(), need, &wb->h, &why);
    }
    if (!ok) {
        LOGW("xact: streaming wave bank in %s at %lld not read: %s", path.c_str(),
             (long long)wb->base, why.empty() ? "read failed" : why.c_str());
        return;
    }
    add_wave_bank(o, std::move(wb));
    notify(nullptr, N_WAVEBANKPREPARED, 0, view, 0, XACTINDEX_INVALID);
}
void E_PrepareWave(X86 *c) {
    XT(8);
    if (ComObj *o = make(c, IF_XACT_WAVE, arg(c, 7))) {
        auto in = std::make_unique<Instance>();
        in->is_wave = true;
        E().instances[o->id] = std::move(in);
    }
}
// PrepareInMemoryWave(this, dwFlags, WAVEBANKENTRY entry (24 bytes by value),
// pdwSeekTable, pbWaveData, dwPlayOffset, nLoopCount, ppWave): 12 dwords.
void E_PrepareInMemoryWave(X86 *c) {
    XT(12);
    if (ComObj *o = make(c, IF_XACT_WAVE, arg(c, 11))) {
        auto in = std::make_unique<Instance>();
        in->is_wave = true;
        E().instances[o->id] = std::move(in);
    }
}
// PrepareStreamingWave(this, dwFlags, WAVEBANKENTRY (24 bytes),
// XACT_STREAMING_PARAMETERS (12 bytes), dwAlignment, pdwSeekTable,
// dwPlayOffset, nLoopCount, ppWave): 15 dwords.
void E_PrepareStreamingWave(X86 *c) {
    XT(15);
    if (ComObj *o = make(c, IF_XACT_WAVE, arg(c, 14))) {
        auto in = std::make_unique<Instance>();
        in->is_wave = true;
        E().instances[o->id] = std::move(in);
    }
}
// XACT_NOTIFICATION_DESCRIPTION, packed: type, flags, sound bank, wave bank,
// cue, wave, cue index, wave index, context.
void E_RegisterNotification(X86 *c) {
    XT(2);
    uint32_t p = arg(c, 1);
    if (!p || !gm_valid(p, 26)) {
        set_eax(c, E_INVALIDARG);
        return;
    }
    Registration r{rd8(p),       rd8(p + 1),   rd32(p + 2), rd32(p + 6),
                   rd32(p + 10), rd16(p + 18), rd32(p + 22)};
    E().regs.push_back(r);
    set_eax(c, S_OK);
}
void E_UnRegisterNotification(X86 *c) {
    uint32_t p = arg(c, 1);
    if (p && gm_valid(p, 26)) {
        auto &regs = E().regs;
        for (size_t i = 0; i < regs.size();)
            if (regs[i].type == rd8(p) && regs[i].cue == rd32(p + 10) &&
                regs[i].sound_bank == rd32(p + 2))
                regs.erase(regs.begin() + (long)i);
            else
                ++i;
    }
    set_eax(c, S_OK);
}
void E_GetCategory(X86 *c) {
    XT(2);
    uint32_t name = arg(c, 1);
    if (!E().settings_parsed || !name || !gm_valid(name, 1)) {
        set_eax(c, 0);
        return;
    }
    int i = E().settings.find_category(gm_str(name));
    set_eax(c, i < 0 ? XACTINDEX_INVALID : (uint32_t)i);
}
uint16_t cue_category(const Instance &in) {
    const xact::Cue *cue = cue_of(in);
    return cue ? cue->category : XACTINDEX_INVALID;
}
// Stop(category, flags), SetVolume(category, volume) and Pause(category,
// pause) reach every cue in the category or below it.
bool in_category(const Instance &in, uint16_t cat) {
    const auto &cats = E().settings.categories;
    uint16_t at = cue_category(in);
    for (int depth = 0; depth < 8 && at < cats.size(); ++depth) {
        if (at == cat)
            return true;
        at = cats[at].parent;
    }
    return false;
}
void E_Stop(X86 *c) {
    XT(3);
    uint16_t cat = (uint16_t)arg(c, 1);
    each_instance([&](Instance &i) {
        if (in_category(i, cat))
            stop(i);
    });
    set_eax(c, S_OK);
}
void E_SetVolume(X86 *c) {
    XT(3);
    E().category_gain[(uint16_t)arg(c, 1)] = argf(c, 2);
    each_instance([](Instance &i) { apply_mix(i); });
    set_eax(c, S_OK);
}
void E_Pause(X86 *c) {
    XT(3);
    uint16_t cat = (uint16_t)arg(c, 1);
    bool on = arg(c, 2) != 0;
    E().category_paused[cat] = on;
    each_instance([&](Instance &i) {
        if (in_category(i, cat))
            pause(i, on);
    });
    set_eax(c, S_OK);
}
int find_var(uint32_t name) {
    if (!E().settings_parsed || !name || !gm_valid(name, 1))
        return -2;
    return E().settings.find_variable(gm_str(name));
}
void E_GetGlobalVariableIndex(X86 *c) {
    int i = find_var(arg(c, 1));
    set_eax(c, i == -2 ? 0 : i < 0 ? XACTINDEX_INVALID : (uint32_t)i);
}
void E_SetGlobalVariable(X86 *c) {
    E().globals[(uint16_t)arg(c, 1)] = argf(c, 2);
    each_instance([](Instance &i) { apply_mix(i); });
    set_eax(c, S_OK);
}
void E_GetGlobalVariable(X86 *c) {
    Instance none;
    float v = var_value(none, (uint16_t)arg(c, 1));
    uint32_t bits;
    memcpy(&bits, &v, 4);
    put(arg(c, 2), bits);
    set_eax(c, S_OK);
}

// --- IXACTSoundBank -----------------------------------------------------
SoundBankData *sound_bank(X86 *c, ComObj **obj = nullptr) {
    ComObj *o = com_this_arg(c);
    if (obj)
        *obj = o;
    if (!o)
        return nullptr;
    auto it = E().sound_banks.find(o->id);
    return it == E().sound_banks.end() ? nullptr : it->second.get();
}
void SB_GetCueIndex(X86 *c) {
    XT(2);
    SoundBankData *sb = sound_bank(c);
    uint32_t name = arg(c, 1);
    if (!name || !gm_valid(name, 1)) {
        set_eax(c, XACTINDEX_INVALID);
        return;
    }
    if (!sb || !sb->parsed) {
        set_eax(c, 0); // an unread bank: any cue is cue 0, as before
        return;
    }
    int i = sb->sb.find_cue(gm_str(name));
    set_eax(c, i < 0 ? XACTINDEX_INVALID : (uint32_t)i);
}
void SB_GetNumCues(X86 *c) {
    SoundBankData *sb = sound_bank(c);
    put(arg(c, 1), sb && sb->parsed ? (uint32_t)sb->sb.cues.size() : 1);
    set_eax(c, S_OK);
}
void SB_GetCueProperties(X86 *c) {
    zero_bytes(arg(c, 2), 0x9c); // XACT_CUE_PROPERTIES, packed
    set_eax(c, S_OK);
}
// A new cue instance of the bank's `index`th cue.
std::unique_ptr<Instance> new_cue(ComObj *bank, uint16_t index) {
    auto in = std::make_unique<Instance>();
    in->sound_bank = bank ? bank->id : 0;
    in->cue = index;
    return in;
}
void SB_Prepare(X86 *c) {
    XT(5);
    ComObj *bank = nullptr;
    sound_bank(c, &bank);
    uint16_t index = (uint16_t)arg(c, 1);
    uint32_t out = arg(c, 4);
    ComObj *o = make(c, IF_XACT_CUE, out);
    if (!o)
        return;
    E().instances[o->id] = new_cue(bank, index);
    notify(nullptr, N_CUEPREPARED, bank ? bank->views[IF_XACT_SOUNDBANK] : 0, 0, rd32(out), index);
}
// Play's ppCue is optional: a fire-and-forget cue needs no object.
void SB_Play(X86 *c) {
    XT(5);
    ComObj *bank = nullptr;
    sound_bank(c, &bank);
    uint16_t index = (uint16_t)arg(c, 1);
    uint32_t out = arg(c, 4);
    auto in = new_cue(bank, index);
    play(*in);
    if (!out) {
        if (in->state == XACT_STATE_PLAYING)
            E().orphans.push_back(std::move(in));
        set_eax(c, S_OK);
        return;
    }
    if (ComObj *o = make(c, IF_XACT_CUE, out))
        E().instances[o->id] = std::move(in);
    else
        stop(*in);
}
void SB_Stop(X86 *c) {
    XT(3);
    ComObj *bank = com_this_arg(c);
    uint16_t index = (uint16_t)arg(c, 1);
    each_instance([&](Instance &i) {
        if (bank && i.sound_bank == bank->id && i.cue == index)
            stop(i);
    });
    set_eax(c, S_OK);
}
void SB_Destroy(X86 *c) {
    ComObj *o = com_this_arg(c);
    if (o)
        notify(c, N_SOUNDBANKDESTROYED, o->views[IF_XACT_SOUNDBANK], 0, 0, XACTINDEX_INVALID);
    destroy(c);
}
void state_prepared(X86 *c) {
    put(arg(c, 1), XACT_STATE_PREPARED);
    set_eax(c, S_OK);
}

// --- IXACTWaveBank ------------------------------------------------------
WaveBankData *wave_bank(X86 *c) {
    ComObj *o = com_this_arg(c);
    auto it = o ? E().wave_banks.find(o->id) : E().wave_banks.end();
    return it == E().wave_banks.end() ? nullptr : it->second.get();
}
void WB_GetNumWaves(X86 *c) {
    WaveBankData *wb = wave_bank(c);
    put(arg(c, 1), wb ? (uint32_t)wb->h.waves.size() : 1);
    set_eax(c, S_OK);
}
void WB_GetWaveIndex(X86 *c) {
    set_eax(c, arg(c, 1) ? 0 : XACTINDEX_INVALID);
}
void WB_GetWaveProperties(X86 *c) {
    zero_bytes(arg(c, 2), 0x40);
    set_eax(c, S_OK);
}
// Prepare/Play(this, waveIndex, flags, playOffset, loopCount, ppWave).
void wave_from_bank(X86 *c, bool and_play) {
    ComObj *b = com_this_arg(c);
    auto it = b ? E().wave_banks.find(b->id) : E().wave_banks.end();
    ComObj *o = make(c, IF_XACT_WAVE, arg(c, 5));
    if (!o)
        return;
    auto in = std::make_unique<Instance>();
    in->is_wave = true;
    if (it != E().wave_banks.end())
        in->wave_bank = it->second;
    in->wave_index = (uint16_t)arg(c, 1);
    in->wave_loops = (uint8_t)arg(c, 4);
    if (and_play)
        play(*in);
    E().instances[o->id] = std::move(in);
}
void WB_Prepare(X86 *c) {
    XT(6);
    wave_from_bank(c, false);
}
void WB_Play(X86 *c) {
    XT(6);
    wave_from_bank(c, true);
}
void WB_Stop(X86 *c) {
    ComObj *b = com_this_arg(c);
    auto it = b ? E().wave_banks.find(b->id) : E().wave_banks.end();
    uint16_t index = (uint16_t)arg(c, 1);
    if (it != E().wave_banks.end())
        each_instance([&](Instance &i) {
            if (i.is_wave && i.wave_bank == it->second && i.wave_index == index)
                stop(i);
        });
    set_eax(c, S_OK);
}
void WB_Destroy(X86 *c) {
    ComObj *o = com_this_arg(c);
    if (o)
        notify(c, N_WAVEBANKDESTROYED, 0, o->views[IF_XACT_WAVEBANK], 0, XACTINDEX_INVALID);
    destroy(c);
}

// --- IXACTCue and IXACTWave --------------------------------------------
// A cue or wave is PREPARED from Prepare until Play, PLAYING while its sound
// lasts and STOPPED after. Games wait on all three: Bully's cutscene loader
// will not call a cutscene loaded until its sound cue reports PREPARED, and
// its speech waits end on STOPPED.
void get_state(X86 *c) {
    XT(2);
    Instance *in = inst(c);
    if (in)
        update(*in);
    uint32_t s = in ? in->state : XACT_STATE_STOPPED;
    if (in && in->paused && s == XACT_STATE_PLAYING)
        s |= XACT_STATE_PAUSED;
    put(arg(c, 1), s);
    set_eax(c, S_OK);
}
void I_Play(X86 *c) {
    XT(1);
    ComObj *o = com_this_arg(c);
    if (Instance *in = inst(c)) {
        play(*in);
        if (!in->is_wave)
            notify(nullptr, N_CUEPLAY, 0, 0, o->views[IF_XACT_CUE], in->cue);
    }
    set_eax(c, S_OK);
}
void I_Stop(X86 *c) {
    XT(2);
    ComObj *o = com_this_arg(c);
    if (Instance *in = inst(c)) {
        bool was = in->state == XACT_STATE_PLAYING;
        stop(*in);
        if (was && !in->is_wave)
            notify(nullptr, N_CUESTOP, 0, 0, o->views[IF_XACT_CUE], in->cue);
    }
    set_eax(c, S_OK);
}
void I_Pause(X86 *c) {
    if (Instance *in = inst(c))
        pause(*in, arg(c, 1) != 0);
    set_eax(c, S_OK);
}
void C_Destroy(X86 *c) {
    ComObj *o = com_this_arg(c);
    Instance *in = inst(c);
    if (o && in) {
        ComObj *bank = com_get(in->sound_bank);
        notify(c, N_CUEDESTROYED, bank ? bank->views[IF_XACT_SOUNDBANK] : 0, 0,
               o->views[IF_XACT_CUE], in->cue);
    }
    destroy(c);
}
// SetMatrixCoefficients(srcCount, dstCount, matrix): the level of source s in
// destination d at [d * srcCount + s]. Its two front channels give the
// volume and the pan.
void I_SetMatrix(X86 *c) {
    XT(4);
    Instance *in = inst(c);
    uint32_t src = arg(c, 1), dst = arg(c, 2), m = arg(c, 3);
    if (in && src && src <= 8 && dst && dst <= 8 && m && gm_valid(m, 4 * src * dst)) {
        float l = 0, r = 0;
        for (uint32_t s = 0; s < src; ++s) {
            uint32_t a = rd32(m + 4 * s), b = dst > 1 ? rd32(m + 4 * (src + s)) : a;
            float fa, fb;
            memcpy(&fa, &a, 4);
            memcpy(&fb, &b, 4);
            l += fa > 0 ? fa : 0;
            r += fb > 0 ? fb : 0;
        }
        l /= src;
        r /= src;
        // A source spread evenly over two speakers is at full level.
        float g = (l > r ? l : r) * (dst > 1 ? 1.0f : 1.0f);
        in->matrix_db = g > 0.00001f ? 20 * log10f(g > 1 ? 1 : g) : -100;
        float pan = 0;
        if (l > 0.00001f && r > 0.00001f)
            pan = 2000 * log10f(r / l);
        else if (l > 0.00001f)
            pan = -10000;
        else if (r > 0.00001f)
            pan = 10000;
        in->pan = pan < -10000 ? -10000 : pan > 10000 ? 10000 : pan;
        apply_mix(*in);
    }
    set_eax(c, S_OK);
}
void C_GetVariableIndex(X86 *c) {
    int i = find_var(arg(c, 1));
    set_eax(c, i == -2 ? 0 : i < 0 ? XACTINDEX_INVALID : (uint32_t)i);
}
void C_SetVariable(X86 *c) {
    XT(3);
    if (Instance *in = inst(c)) {
        in->vars[(uint16_t)arg(c, 1)] = argf(c, 2);
        apply_mix(*in);
    }
    set_eax(c, S_OK);
}
void C_GetVariable(X86 *c) {
    Instance *in = inst(c);
    Instance none;
    float v = var_value(in ? *in : none, (uint16_t)arg(c, 1));
    uint32_t bits;
    memcpy(&bits, &v, 4);
    put(arg(c, 2), bits);
    set_eax(c, S_OK);
}
void C_GetProperties(X86 *c) {
    put(arg(c, 1), 0);
    set_eax(c, E_FAIL);
}
// IXACTWave SetPitch(XACTPITCH, cents) and SetVolume(XACTVOLUME, linear).
void W_SetPitch(X86 *c) {
    if (Instance *in = inst(c)) {
        in->wave_cents = (float)(int16_t)arg(c, 1);
        apply_mix(*in);
    }
    set_eax(c, S_OK);
}
void W_SetVolume(X86 *c) {
    if (Instance *in = inst(c)) {
        float g = argf(c, 1);
        in->wave_db = g > 0.00001f ? 20 * log10f(g) : -100;
        apply_mix(*in);
    }
    set_eax(c, S_OK);
}

const ComMethod g_engine[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetRendererCount", 2, E_GetRendererCount},
    {"GetRendererDetails", 3, E_GetRendererDetails},
    {"GetFinalMixFormat", 2, E_GetFinalMixFormat},
    {"Initialize", 2, E_Initialize},
    {"ShutDown", 1, E_ShutDown},
    {"DoWork", 1, E_DoWork},
    {"CreateSoundBank", 6, E_CreateSoundBank},
    {"CreateInMemoryWaveBank", 6, E_CreateInMemoryWaveBank},
    {"CreateStreamingWaveBank", 3, E_CreateStreamingWaveBank},
    {"PrepareWave", 8, E_PrepareWave},
    {"PrepareInMemoryWave", 12, E_PrepareInMemoryWave},
    {"PrepareStreamingWave", 15, E_PrepareStreamingWave},
    {"RegisterNotification", 2, E_RegisterNotification},
    {"UnRegisterNotification", 2, E_UnRegisterNotification},
    {"GetCategory", 2, E_GetCategory},
    {"Stop", 3, E_Stop},
    {"SetVolume", 3, E_SetVolume},
    {"Pause", 3, E_Pause},
    {"GetGlobalVariableIndex", 2, E_GetGlobalVariableIndex},
    {"SetGlobalVariable", 3, E_SetGlobalVariable},
    {"GetGlobalVariable", 3, E_GetGlobalVariable},
};
const ComMethod g_soundbank[] = {
    {"GetCueIndex", 2, SB_GetCueIndex},
    {"GetNumCues", 2, SB_GetNumCues},
    {"GetCueProperties", 3, SB_GetCueProperties},
    {"Prepare", 5, SB_Prepare},
    {"Play", 5, SB_Play},
    {"Stop", 3, SB_Stop},
    {"Destroy", 1, SB_Destroy},
    {"GetState", 2, state_prepared},
};
const ComMethod g_wavebank[] = {
    {"Destroy", 1, WB_Destroy},
    {"GetNumWaves", 2, WB_GetNumWaves},
    {"GetWaveIndex", 2, WB_GetWaveIndex},
    {"GetWaveProperties", 3, WB_GetWaveProperties},
    {"Prepare", 6, WB_Prepare},
    {"Play", 6, WB_Play},
    {"Stop", 3, WB_Stop},
    {"GetState", 2, state_prepared},
};
const ComMethod g_cue[] = {
    {"Play", 1, I_Play},
    {"Stop", 2, I_Stop},
    {"GetState", 2, get_state},
    {"Destroy", 1, C_Destroy},
    {"SetMatrixCoefficients", 4, I_SetMatrix},
    {"GetVariableIndex", 2, C_GetVariableIndex},
    {"SetVariable", 3, C_SetVariable},
    {"GetVariable", 3, C_GetVariable},
    {"Pause", 2, I_Pause},
    {"GetProperties", 2, C_GetProperties},
};
const ComMethod g_wave[] = {
    {"Destroy", 1, destroy},
    {"Play", 1, I_Play},
    {"Stop", 2, I_Stop},
    {"Pause", 2, I_Pause},
    {"GetState", 2, get_state},
    {"SetPitch", 2, W_SetPitch},
    {"SetVolume", 2, W_SetVolume},
    {"SetMatrixCoefficients", 4, I_SetMatrix},
    {"GetProperties", 2, C_GetProperties},
};

} // namespace

void xact_frame_pump(X86 *c) {
    pump(c);
}

void xact_reset() {
    Engine &e = E();
    each_instance([](Instance &i) { stop_voice(i.v); });
    e = Engine();
}

XactCounters xact_counters() {
    const Engine &e = E();
    return {e.plays, e.streamed, e.missing, e.undecodable};
}

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
    com_set_destructor(K_XACT, on_destroy);
    com_register_iid(IF_XACT_ENGINE, IID_IXACTEngine_);
    com_register_class(CLSID_XACTEngine_, "XACTEngine", IF_XACT_ENGINE, create_engine);
    com_register_class(CLSID_XACTDebugEngine_, "XACTDebugEngine", IF_XACT_ENGINE, create_engine);
}
