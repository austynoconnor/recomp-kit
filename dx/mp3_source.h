// Shared frame-at-a-time MPEG audio decoder. Owns its encoded input and
// returns interleaved signed 16-bit PCM; it never accesses guest memory.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <vector>
#include "../third_party/minimp3/minimp3.h"

struct Mp3Source {
    bool open(const std::vector<uint8_t> &bytes);
    bool decode_next(std::vector<int16_t> &out);
    uint32_t rate() const {
        return rate_;
    }
    uint32_t channels() const {
        return channels_;
    }
    void seek_frames(uint64_t target);
    bool drained() const {
        return drained_;
    }
    // DirectShow exposes duration and the frame-aligned seek position.
    uint64_t position_frames() const {
        return position_;
    }
    int64_t duration_frames();
    // Streaming input (a movie's audio, demultiplexed as the file is read):
    // begin_stream() empties the source, append() adds encoded bytes and
    // finish() says no more will come. Until then running out of input is a
    // pause, not the end: drained() stays false and decoding resumes once more
    // bytes arrive. open() probes the format from what has been appended.
    void begin_stream();
    void append(const uint8_t *p, size_t n);
    void finish();
    bool streaming() const {
        return streaming_ && !finished_;
    }
    size_t unread_bytes() const {
        return bytes_.size() - offset_;
    }
    bool open_streamed();

  private:
    bool decode_frame(std::vector<int16_t> &out);
    std::vector<uint8_t> bytes_;
    std::vector<int16_t> first_;
    size_t offset_ = 0;
    mp3dec_t dec_{};
    uint32_t rate_ = 0, channels_ = 0;
    uint64_t position_ = 0;
    int64_t duration_ = -1;
    bool drained_ = true;
    bool streaming_ = false, finished_ = true;
};
