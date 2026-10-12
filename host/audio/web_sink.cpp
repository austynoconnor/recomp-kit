// web_sink.cpp - the mixer's output in a browser: an AudioWorklet reading a
// ring buffer in the WebAssembly memory, filled by a thread of the sink's own.
//
// SDL's Emscripten backend plays through a ScriptProcessorNode, whose
// callback runs on the page's main thread and renders the mix there. That
// thread also records every Direct3D frame into WebGPU and presents, and the
// mixer's render lock is shared with the game thread, so a busy or blocked
// main thread starved the output: crackles and repeats. Here the audio
// rendering thread of the browser only copies finished samples out of the
// ring, and the mix is rendered on a worker thread that may block freely.
#ifdef __EMSCRIPTEN__
#include "sink.h"

#include <emscripten.h>
#include <emscripten/threading.h>

#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "../../platform/os.h"

// Opens the AudioContext and its worklet node on the main thread; returns
// the context's sample rate, or 0.
// clang-format off: the body is JavaScript, which clang-format would split
// ("!==" into "!= =").
EM_JS(int, recomp_web_audio_open,
      (double rate, const char *src, uint32_t header, uint32_t samples, uint32_t capacity,
       uint32_t prefill),
      {
          try {
              var ctx;
              try {
                  ctx = new AudioContext({sampleRate : rate, latencyHint : 'interactive'});
              } catch (e) {
                  ctx = new AudioContext({latencyHint : 'interactive'});
              }
              Module['recompAudio'] = ctx;
              var url = URL.createObjectURL(new Blob([UTF8ToString(src)], {
                  type:
                      'application/javascript'
              }));
              ctx.audioWorklet.addModule(url)
                  .then(function() {
                      var node = new AudioWorkletNode(ctx, 'recomp-ring-player', {
                          numberOfInputs : 0,
                          numberOfOutputs : 1,
                          outputChannelCount : [2],
                          processorOptions : {
                              buffer : (typeof wasmMemory !== 'undefined' ? wasmMemory.buffer
                                                                           : HEAPU8.buffer),
                              header : header,
                              samples : samples,
                              capacity : capacity,
                              prefill : prefill
                          }
                      });
                      node.connect(ctx.destination);
                      Module['recompAudioNode'] = node;
                  })
                  .catch(function(e) { console.error('audio worklet: ' + e); });
              // Autoplay: start on the first gesture this frame sees, or as
              // soon as the page has had one.
              var resume = function() {
                  if (ctx.state === 'suspended')
                      ctx.resume();
              };
              [ 'pointerdown', 'keydown', 'touchstart', 'gamepadconnected' ].forEach(
                  function(t) { window.addEventListener(t, resume, true); });
              Module['recompAudioTimer'] = setInterval(
                  function() {
                      if (ctx.state === 'running') {
                          clearInterval(Module['recompAudioTimer']);
                          return;
                      }
                      if (!navigator.userActivation || navigator.userActivation.hasBeenActive)
                          resume();
                  },
                  250);
              resume();
              return ctx.sampleRate | 0;
          } catch (e) {
              console.error('audio: ' + e);
              return 0;
          }
      });
// clang-format on

namespace {

// Read by the worklet through Int32/Float32 views of the same memory: keep
// the field order in step with kWorklet's indices (u32[hdr + 0..4]).
struct alignas(16) RingHeader {
    std::atomic<uint32_t> write{0};     // frames written, ever (wraps)
    std::atomic<uint32_t> read{0};      // frames the worklet has taken, ever
    std::atomic<uint32_t> underruns{0}; // render quanta the worklet had to pad
    std::atomic<uint32_t> paused{0};    // 1: play silence, count nothing
    std::atomic<uint32_t> quanta{0};    // render quanta played
};

// The processor. Samples are interleaved stereo; capacity is a power of two.
// It waits for `prefill` frames before it starts, and again after running dry.
const char *const kWorklet = R"JS(
class RecompRingPlayer extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const o = options.processorOptions;
    this.u32 = new Uint32Array(o.buffer);
    this.f32 = new Float32Array(o.buffer);
    this.hdr = o.header >>> 2;
    this.smp = o.samples >>> 2;
    this.mask = o.capacity - 1;
    this.prefill = o.prefill;
    this.primed = false;
  }
  process(inputs, outputs) {
    const out = outputs[0];
    const left = out[0], right = out[1] || out[0], n = left.length;
    const u32 = this.u32, h = this.hdr;
    Atomics.add(u32, h + 4, 1);
    const w = Atomics.load(u32, h), r = Atomics.load(u32, h + 1);
    const avail = (w - r) >>> 0;
    if (Atomics.load(u32, h + 3) || (!this.primed && avail < this.prefill)) {
      left.fill(0); right.fill(0);
      return true;
    }
    this.primed = true;
    const take = Math.min(avail, n), f32 = this.f32, base = this.smp, mask = this.mask;
    let at = r & mask;
    for (let i = 0; i < take; ++i) {
      left[i] = f32[base + 2 * at];
      right[i] = f32[base + 2 * at + 1];
      at = (at + 1) & mask;
    }
    if (take < n) {
      left.fill(0, take); right.fill(0, take);
      Atomics.add(u32, h + 2, 1);
      this.primed = false;
    }
    Atomics.store(u32, h + 1, (r + take) >>> 0);
    return true;
  }
}
registerProcessor('recomp-ring-player', RecompRingPlayer);
)JS";

class WebSink final : public AudioSink {
  public:
    ~WebSink() override {
        stop();
    }
    bool start(double rate, std::function<void(float *, float *, uint32_t)> render) override {
        if (running_)
            return true;
        render_ = std::move(render);
        // The backlog kept rendered ahead of the worklet, in frames:
        // RECOMP_AUDIO_AHEAD overrides 4096 (85 ms at 48 kHz). A browser tab's
        // workers can be descheduled for tens of milliseconds at a time.
        ahead_ = 4096;
        if (const char *ahead = recomp_env("AUDIO_AHEAD"); ahead && *ahead)
            ahead_ = std::max<uint32_t>(512, uint32_t(strtoul(ahead, nullptr, 10)));
        capacity_ = 1;
        while (capacity_ < ahead_ * 2)
            capacity_ <<= 1;
        samples_.assign(size_t(capacity_) * 2, 0.0f);
        rate_ = rate;
        running_ = true;
        // Web Audio objects belong to the page's main thread.
        const int ok = emscripten_sync_run_in_main_runtime_thread(
            EM_FUNC_SIG_II, &WebSink::open_on_main, int(reinterpret_cast<uintptr_t>(this)));
        if (ok <= 0) {
            running_ = false;
            fprintf(stderr, "[host] audio: no Web Audio output\n");
            return false;
        }
        if (ok != int(rate))
            fprintf(stderr,
                    "[host] audio: the browser runs Web Audio at %d Hz, not %.0f Hz; the "
                    "mix will play at the wrong pitch\n",
                    ok, rate);
        printf("[host] audio device running: mixer %.0f Hz stereo, Web Audio worklet, "
               "%u frames ahead\n",
               rate, ahead_);
        fflush(stdout);
        producer_ = std::thread([this] { produce(); });
        return true;
    }
    void pause(bool paused) override {
        header_.paused.store(paused ? 1 : 0, std::memory_order_release);
    }
    void stop() override {
        if (!running_)
            return;
        running_ = false;
        if (producer_.joinable())
            producer_.join();
        MAIN_THREAD_EM_ASM({
            var ctx = Module['recompAudio'];
            if (Module['recompAudioTimer'])
                clearInterval(Module['recompAudioTimer']);
            if (ctx)
                ctx.close();
            Module['recompAudio'] = Module['recompAudioNode'] = undefined;
        });
        report();
    }
    bool running() const override {
        return running_;
    }

  private:
    static int open_on_main(int self) {
        auto *sink = reinterpret_cast<WebSink *>(uintptr_t(uint32_t(self)));
        return recomp_web_audio_open(sink->rate_, kWorklet, uint32_t(uintptr_t(&sink->header_)),
                                     uint32_t(uintptr_t(sink->samples_.data())), sink->capacity_,
                                     sink->ahead_ / 2);
    }
    // Evidence for audio reports: underruns are worklet quanta (128 frames)
    // that found the ring short; a slow render is the mix itself.
    void report() {
        printf("[host] audio sink: %u quanta played, %u underruns, %llu renders, max render "
               "%.1f ms, max queued %u / min queued %u frames, ahead %u\n",
               header_.quanta.load(), header_.underruns.load(), (unsigned long long)renders_,
               max_render_ms_, max_queued_, min_queued_ == ~0u ? 0 : min_queued_, ahead_);
        fflush(stdout);
        max_queued_ = 0;
        min_queued_ = ~0u;
    }
    void produce() {
        std::vector<float> left, right;
        const uint32_t mask = capacity_ - 1;
        auto last_report = std::chrono::steady_clock::now();
        bool seen_read = false;
        while (running_) {
            const uint32_t w = header_.write.load(std::memory_order_relaxed);
            const uint32_t r = header_.read.load(std::memory_order_acquire);
            const uint32_t queued = w - r;
            if (r)
                seen_read = true;
            if (seen_read) {
                max_queued_ = std::max(max_queued_, queued);
                min_queued_ = std::min(min_queued_, queued);
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - last_report >= std::chrono::seconds(30)) {
                last_report = now;
                report();
            }
            if (header_.paused.load(std::memory_order_acquire) || queued >= ahead_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            // Render in blocks of at most 512 frames so the ring is topped up
            // steadily rather than in long bursts behind the render lock.
            const uint32_t frames = std::min<uint32_t>(ahead_ - queued, 512);
            left.assign(frames, 0.0f);
            right.assign(frames, 0.0f);
            const auto t0 = std::chrono::steady_clock::now();
            render_(left.data(), right.data(), frames);
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                    .count();
            max_render_ms_ = std::max(max_render_ms_, ms);
            ++renders_;
            for (uint32_t i = 0; i < frames; ++i) {
                const uint32_t at = (w + i) & mask;
                samples_[size_t(at) * 2] = left[i];
                samples_[size_t(at) * 2 + 1] = right[i];
            }
            header_.write.store(w + frames, std::memory_order_release);
        }
    }

    RingHeader header_;
    std::vector<float> samples_;
    uint32_t capacity_ = 0, ahead_ = 4096;
    double rate_ = 0;
    std::atomic<bool> running_{false};
    std::thread producer_;
    std::function<void(float *, float *, uint32_t)> render_;
    uint64_t renders_ = 0;
    double max_render_ms_ = 0;
    uint32_t max_queued_ = 0, min_queued_ = ~0u;
};
} // namespace

std::unique_ptr<AudioSink> make_web_sink() {
    return std::make_unique<WebSink>();
}
#endif
