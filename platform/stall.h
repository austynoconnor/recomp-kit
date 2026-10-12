// stall.h - where a long game frame went.
//
// The guest runs one thread at a time (the scheduler's baton), so a host call
// that blocks - a file read waiting for a download, a wait for the browser's
// main thread - holds up every guest thread, not just the caller. Those calls
// add what they waited to a category here; the game's present closes the
// frame, and a frame longer than the threshold is reported on stderr (the
// page console in a browser) with the share each category took. Whatever no
// category claims is the game's own work, or a machine too busy (or paging)
// to run it.
#pragma once

#include <cstdint>

enum RecompStallKind : int {
    RECOMP_STALL_FILE = 0, // file opens and reads
    RECOMP_STALL_RENDER,   // waits for the render or browser main thread
    RECOMP_STALL_KINDS
};

// Thread-safe. Adds `seconds` waited in `kind`.
void recomp_stall_add(RecompStallKind kind, double seconds);

// Measures a scope into `kind`.
struct RecompStallScope {
    explicit RecompStallScope(RecompStallKind kind);
    ~RecompStallScope();
    RecompStallScope(const RecompStallScope &) = delete;
    RecompStallScope &operator=(const RecompStallScope &) = delete;

  private:
    RecompStallKind kind_;
    uint64_t start_ns_;
};

// The game presented a frame. Reports it when it took longer than the
// threshold (RECOMP_STALL_MS, default 100; 0 turns reports off).
void recomp_stall_frame();

struct RecompStallStats {
    uint64_t frames = 0, stalls = 0;
    double last_ms = 0, worst_ms = 0;
    // The category that took most of the last stall: "FILE", "RENDER" or "GAME".
    const char *last_cause = "";
};
RecompStallStats recomp_stall_stats();
