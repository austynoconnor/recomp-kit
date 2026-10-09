// A media file the guest asked Media Foundation to play, decoded on the host
// with FFmpeg. Video comes out as opaque ARGB frames with the time each is
// due; audio as interleaved signed 16-bit samples. Nothing here touches guest
// memory: the Media Foundation objects above it own that side.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mf {

struct VideoFrame {
    int32_t width = 0, height = 0;
    double pts = 0;             // seconds from the start of the stream
    std::vector<uint32_t> argb; // width * height, top row first
};

class Media {
  public:
    Media() = default;
    ~Media();
    Media(const Media &) = delete;
    Media &operator=(const Media &) = delete;

    // Opens `path` (a host path) and reads its stream layout. False leaves the
    // object closed and, when `why` is given, says what the decoder refused.
    bool open(const std::string &path, std::string *why = nullptr);
    void close();
    bool is_open() const;

    bool has_video() const;
    bool has_audio() const;
    int32_t width() const;
    int32_t height() const;
    double duration() const; // seconds; 0 when the container does not say
    int32_t audio_rate() const;
    int32_t audio_channels() const;

    // Decodes until the next video frame is ready, appending any audio decoded
    // on the way. False means the file ended (or there is no video at all).
    bool next_video(VideoFrame *out);
    // Decodes audio until at least `samples` interleaved samples are queued or
    // the file ends. Audio-only files need this; with video, next_video feeds
    // the same queue.
    void fill_audio(size_t samples);
    // Hands over everything decoded so far, leaving the queue empty.
    std::vector<int16_t> take_audio();
    bool finished() const;

    // Opaque to callers; the decoder file defines it.
    struct State;

  private:
    State *s_ = nullptr;
};

// ---------------------------------------------------------------------------
// A bare MPEG-1 video elementary stream (sequence header, GOPs, pictures, no
// container), as DirectShow's MPEG Video Decoder would take it from a file
// source. Metal Gear Solid 2 keeps its movies like this in cdrom.img/pac/.
// ---------------------------------------------------------------------------
struct Mpeg1Info {
    int32_t width = 0, height = 0;
    double fps = 0;       // from the sequence header's frame-rate code
    int64_t pictures = 0; // picture start codes in the stream
};

// Reads the first sequence header and counts the pictures. False when the
// bytes do not start with an MPEG-1 sequence header or name no known rate.
bool mpeg1_scan(const uint8_t *data, size_t size, Mpeg1Info *out);

// Every sequence header's aspect-ratio code set to 1 (square pixels). Konami's
// streams carry codes FFmpeg refuses in some headers; the aspect is a display
// hint that nothing here uses, so normalising it costs nothing. Returns the
// number of headers changed.
int mpeg1_fix_aspect(std::vector<uint8_t> &bytes);

class Mpeg1Stream {
  public:
    Mpeg1Stream() = default;
    ~Mpeg1Stream();
    Mpeg1Stream(const Mpeg1Stream &) = delete;
    Mpeg1Stream &operator=(const Mpeg1Stream &) = delete;

    // Takes the whole stream (headers already fixed). False when this build
    // has no MPEG-1 decoder or the bytes are not a stream.
    bool open(std::vector<uint8_t> bytes, std::string *why = nullptr);
    bool is_open() const;
    const Mpeg1Info &info() const {
        return info_;
    }
    // The next picture in display order; false at the end.
    bool next(VideoFrame *out);
    // Back to the first picture.
    bool rewind();
    // Pictures handed out since open or the last rewind.
    int64_t decoded() const {
        return decoded_;
    }

    struct State;

  private:
    bool start();
    State *s_ = nullptr;
    std::vector<uint8_t> bytes_;
    Mpeg1Info info_;
    int64_t decoded_ = 0;
};

} // namespace mf
