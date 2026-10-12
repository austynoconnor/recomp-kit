#include "stall.h"

#include "os.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace {
std::atomic<uint64_t> g_wait_ns[RECOMP_STALL_KINDS];
std::atomic<uint64_t> g_wait_count[RECOMP_STALL_KINDS];
std::atomic<uint64_t> g_longest_ns[RECOMP_STALL_KINDS];
std::mutex g_stats_m;
RecompStallStats g_stats;
uint64_t g_last_frame_ns = 0;

double threshold_ms() {
    static const double ms = [] {
        const char *v = recomp_env("STALL_MS");
        return v && *v ? atof(v) : 100.0;
    }();
    return ms;
}
} // namespace

void recomp_stall_add(RecompStallKind kind, double seconds) {
    if (kind < 0 || kind >= RECOMP_STALL_KINDS || !(seconds > 0))
        return;
    const uint64_t ns = uint64_t(seconds * 1e9);
    g_wait_ns[kind].fetch_add(ns, std::memory_order_relaxed);
    g_wait_count[kind].fetch_add(1, std::memory_order_relaxed);
    uint64_t seen = g_longest_ns[kind].load(std::memory_order_relaxed);
    while (ns > seen &&
           !g_longest_ns[kind].compare_exchange_weak(seen, ns, std::memory_order_relaxed)) {
    }
}

RecompStallScope::RecompStallScope(RecompStallKind kind)
    : kind_(kind), start_ns_(os_monotonic_ns()) {}
RecompStallScope::~RecompStallScope() {
    recomp_stall_add(kind_, double(os_monotonic_ns() - start_ns_) * 1e-9);
}

void recomp_stall_frame() {
    const uint64_t now = os_monotonic_ns();
    uint64_t wait[RECOMP_STALL_KINDS], count[RECOMP_STALL_KINDS], longest[RECOMP_STALL_KINDS];
    for (int k = 0; k < RECOMP_STALL_KINDS; ++k) {
        wait[k] = g_wait_ns[k].exchange(0, std::memory_order_relaxed);
        count[k] = g_wait_count[k].exchange(0, std::memory_order_relaxed);
        longest[k] = g_longest_ns[k].exchange(0, std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> lock(g_stats_m);
    const uint64_t before = g_last_frame_ns;
    g_last_frame_ns = now;
    ++g_stats.frames;
    const double limit = threshold_ms();
    if (!before || limit <= 0)
        return;
    const double ms = double(now - before) * 1e-6;
    if (ms < limit)
        return;
    const double file = double(wait[RECOMP_STALL_FILE]) * 1e-6;
    const double render = double(wait[RECOMP_STALL_RENDER]) * 1e-6;
    double game = ms - file - render;
    if (game < 0)
        game = 0;
    ++g_stats.stalls;
    g_stats.last_ms = ms;
    if (ms > g_stats.worst_ms)
        g_stats.worst_ms = ms;
    g_stats.last_cause = file >= render && file >= game ? "FILE"
                         : render >= game               ? "RENDER"
                                                        : "GAME";
    fprintf(stderr,
            "[stall] frame %llu took %.0f ms: file %.0f ms (%llu calls, longest %.0f ms), "
            "render waits %.0f ms (%llu, longest %.0f ms), game or busy machine %.0f ms\n",
            (unsigned long long)g_stats.frames, ms, file,
            (unsigned long long)count[RECOMP_STALL_FILE], double(longest[RECOMP_STALL_FILE]) * 1e-6,
            render, (unsigned long long)count[RECOMP_STALL_RENDER],
            double(longest[RECOMP_STALL_RENDER]) * 1e-6, game);
}

RecompStallStats recomp_stall_stats() {
    std::lock_guard<std::mutex> lock(g_stats_m);
    return g_stats;
}
