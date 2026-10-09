// xact_banks.h - the XACT 2 file formats, read without an engine.
//
// Three files describe an XACT 2 project (DirectX SDK, August 2007; tool
// version 44). The global settings (XGSF) name the categories and the
// variables and hold the RPC curves that turn a variable into a volume or a
// pitch. A sound bank (SDBK) names the cues and says which wave of which wave
// bank each one plays. A wave bank (WBND) holds the waves themselves, or for a
// streaming bank says where in its file each one lies.
//
// These are plain readers over bytes: nothing here touches guest memory or the
// host mixer, so the tests can hand them banks they build themselves. The
// layouts follow the August 2007 SDK's xact2wb.h for wave banks and the
// published reverse-engineering of the sound bank and settings formats
// (format versions 43 and 42); Bully: Scholarship Edition's banks are the ones
// they have been checked against.
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

namespace xact {

// --- Global settings (XGSF) ---------------------------------------------
struct Category {
    std::string name;
    uint16_t parent = 0xffff;
    float volume_db = 0; // the authored volume
};
struct Variable {
    std::string name;
    uint8_t flags = 0; // 1 public, 2 read-only, 4 per cue instance, 8 reserved
    float initial = 0, min = 0, max = 0;
};
// An RPC curve: a variable's value mapped, piecewise linearly, to a parameter.
// Volume is in hundredths of a decibel and pitch in cents.
struct RpcCurve {
    uint32_t code = 0; // the curve's byte offset in the settings file
    uint16_t variable = 0;
    uint16_t parameter = 0; // 0 volume, 1 pitch, 2 reverb send, 3/4 filter
    std::vector<float> x, y;
    float eval(float v) const;
};
struct Settings {
    std::vector<Category> categories;
    std::vector<Variable> variables;
    std::vector<RpcCurve> curves;
    const RpcCurve *curve(uint32_t code) const;
    int find_category(const std::string &name) const;
    int find_variable(const std::string &name) const;
};
bool parse_settings(const uint8_t *d, size_t n, Settings *out, std::string *error);

// --- Sound bank (SDBK) --------------------------------------------------
// One wave a cue can play: which bank (an index into the sound bank's own
// wave bank names), which wave in it, and how often it loops (255 for ever).
struct Track {
    uint8_t wave_bank = 0;
    uint16_t wave = 0;
    uint8_t loops = 0;
    int16_t pitch_min = 0, pitch_max = 0; // a play's random pitch, in cents
};
struct Cue {
    std::string name;
    uint16_t category = 0;
    float volume_db = 0;       // the sound's and its clip's volume together
    int16_t pitch = 0;         // the sound's own pitch, in cents
    std::vector<Track> tracks; // a play picks one
    std::vector<uint32_t> rpc; // codes of the RPC curves the sound uses
};
struct SoundBank {
    std::string name;
    std::vector<std::string> wave_banks;
    std::vector<Cue> cues;
    int find_cue(const std::string &name) const;
};
bool parse_sound_bank(const uint8_t *d, size_t n, SoundBank *out, std::string *error);
// XACT's own volume byte, in decibels: a fitted curve through the
// authoring tool's table, 0xb4 being 0 dB and 0 being -96 dB.
float volume_byte_db(uint8_t v);

// --- Wave bank (WBND) ---------------------------------------------------
enum WaveTag : uint8_t { TAG_PCM = 0, TAG_XMA = 1, TAG_ADPCM = 2, TAG_WMA = 3 };
struct Wave {
    uint8_t tag = TAG_PCM;
    uint8_t channels = 1;
    uint32_t rate = 0;
    uint32_t block_align = 0; // bytes per block, all channels; PCM: a frame
    uint8_t bits = 16;        // PCM only
    uint32_t offset = 0;      // from the start of the bank's wave data
    uint32_t bytes = 0;
    uint32_t loop_start = 0, loop_samples = 0; // in samples
    uint32_t samples = 0;                      // the duration the bank gives
    uint32_t samples_per_block() const;
};
struct WaveBankHeader {
    std::string name;
    uint32_t flags = 0;       // 1 streaming; 0x20000 compact
    uint32_t data_offset = 0; // of the wave data, from the bank's start
    uint32_t meta_offset = 0, meta_bytes = 0;
    uint32_t bank_offset = 0, bank_bytes = 0;
    std::vector<Wave> waves;
};
// The first 52 bytes: the header and its five segments. `meta_needed` is
// how many bytes from the bank's start parse_wave_bank needs.
bool parse_wave_bank_segments(const uint8_t *d, size_t n, WaveBankHeader *out, size_t *meta_needed,
                              std::string *error);
bool parse_wave_bank(const uint8_t *d, size_t n, WaveBankHeader *out, std::string *error);

// --- Decoding -----------------------------------------------------------
// Decodes whole blocks of MS ADPCM (WAVE_FORMAT_ADPCM with the standard seven
// coefficient pairs) to interleaved 16-bit PCM, appending to `out`. A short
// last block decodes as far as it goes. Returns the sample frames produced.
uint32_t decode_adpcm(const uint8_t *src, size_t bytes, uint32_t block_align, int channels,
                      std::vector<int16_t> *out);

} // namespace xact
