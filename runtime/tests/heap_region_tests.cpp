// Allocator checks need no game inputs, host window or child processes.
#include "../memory.h"
#include "../imports.h"
#include "../thunks.h"
#include "game_config.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <initializer_list>

int main() {
    unsigned checks = 0, failures = 0;
    auto check = [&](bool good, const char *name) {
        ++checks;
        if (!good) {
            ++failures;
            std::printf("FAIL: %s\n", name);
        }
    };
    mem_init();
    const uint32_t low_size = HEAP_LIMIT - HEAP_BASE;
    const uint32_t extra_size = RECOMP_EXTRA_HEAP_SIZE;
    const uint64_t total = uint64_t(low_size) + extra_size;
    check(heap_check().empty(), "initial region coverage");
    check(heap_stats().free_bytes == total, "initial total capacity");
    check(heap_stats().free_blocks == (extra_size ? 2u : 1u), "disjoint free regions");
    // Sentinels in the reserved gap must survive exhaustion and coalescing.
    wr32(HEAP_LIMIT + 256, 0x13579bdf);
    wr32(STACK_TOP - 16, 0x2468ace0);
    uint32_t low = heap_alloc(low_size);
    check(low == HEAP_BASE, "consume original region");
    if (extra_size) {
        uint32_t high = heap_alloc(extra_size);
        check(high == RECOMP_EXTRA_HEAP_BASE, "spill into explicit high region");
        if (high) {
            wr32(high, 0xaabbccdd);
            wr32(high + extra_size - 4, 0xddccbbaa);
            check(rd32(high) == 0xaabbccdd && rd32(high + extra_size - 4) == 0xddccbbaa,
                  "first and last high addresses are usable");
        }
        check(heap_stats().free_bytes == 0, "both regions exhausted");
        check(heap_alloc(16) == 0, "exhaustion fails instead of using reserved gap");
        check(heap_check().empty(), "exhausted map remains consistent");
        check(heap_free(high), "release high region");
    } else {
        check(heap_alloc(16) == 0, "default arena retains its original capacity");
        check(heap_free(low), "release default arena");
        low = 0;
    }
    // A neighbour prevents in-place growth, forcing copy within a usable region.
    uint32_t a = heap_alloc(64), b = heap_alloc(64);
    check(a && b, "allocate adjacent blocks");
    if (a && b) {
        std::memset(g_mem + a, 0x5a, 64);
        uint32_t grown = heap_realloc(a, 256, true);
        check(grown && grown != a, "realloc moves around occupied neighbour");
        if (grown) {
            bool preserved = true, zeroed = true;
            for (unsigned i = 0; i < 64; ++i)
                preserved &= rd8(grown + i) == 0x5a;
            for (unsigned i = 64; i < 256; ++i)
                zeroed &= rd8(grown + i) == 0;
            check(preserved, "realloc preserves original contents");
            check(zeroed, "realloc zeroes growth");
            check(heap_size(a) == 0xffffffffu, "old realloc block released");
            check(heap_free(grown), "release grown block");
        }
        check(heap_free(b), "release neighbour");
    }
    if (low)
        check(heap_free(low), "release original region");
    check(heap_check().empty(), "coalescing respects region gap");
    check(heap_stats().free_bytes == total && heap_stats().used_blocks == 0,
          "all capacity restored");
    check(heap_stats().free_blocks == (extra_size ? 2u : 1u), "regions do not merge across gap");
    check(rd32(HEAP_LIMIT + 256) == 0x13579bdf && rd32(STACK_TOP - 16) == 0x2468ace0,
          "reserved map remains untouched");
    check(heap_alloc(0xffffffffu) == 0, "overflow-sized request refused");
    imports_init();
    auto invoke = [&](const char *name, std::initializer_list<uint32_t> args) {
        X86 c{};
        c.r[R_ESP] = STACK_TOP - 1024;
        wr32(c.r[R_ESP], 0x00401000);
        unsigned i = 0;
        for (uint32_t a : args)
            wr32(c.r[R_ESP] + 4 + 4 * i++, a);
        check(imports_dispatch(&c, imports_resolve("KERNEL32.dll", name)), "memory query dispatch");
        check(c.r[R_ESP] == STACK_TOP - 1024 + 4 + 4 * args.size(), "memory query stdcall cleanup");
        return c.r[R_EAX];
    };
    const uint32_t out = 0x00301000;
    invoke("GetSystemInfo", {out});
    check(rd32(out + 12) == (extra_size ? RECOMP_EXTRA_HEAP_BASE + extra_size - 1 : HEAP_LIMIT),
          "system info exposes configured maximum address");
    if (extra_size) {
        std::memset(g_mem + out, 0xa5, 32);
        check(invoke("VirtualQuery", {RECOMP_EXTRA_HEAP_BASE + 123, out, 28}) == 28 &&
                  rd32(out + 4) == RECOMP_EXTRA_HEAP_BASE && rd32(out + 16) == 0x1000 &&
                  rd32(out + 24) == 0x20000 && rd32(out + 28) == 0xa5a5a5a5,
              "high heap is committed private memory with bounded output");
        check(invoke("VirtualQuery", {HEAP_LIMIT + 256, out, 28}) == 28 &&
                  rd32(out + 16) == 0x10000,
              "reserved gap is not classified as high heap");
        uint32_t target =
            imports_alloc_trampoline("TEST", "HighHeapThunk", [](X86 *c) { c->r[R_EAX] += 1; }, 0);
        uint32_t high = RECOMP_EXTRA_HEAP_BASE;
        wr8(high, 0xb8); // MOV EAX, immediate; JMP import trampoline.
        wr32(high + 1, 0x13579bdf);
        wr8(high + 5, 0xe9);
        wr32(high + 6, target - high - 10);
        X86 c{};
        c.r[R_ESP] = STACK_TOP - 1024;
        wr32(c.r[R_ESP], 0x00401000);
        check(recomp_run_thunk(&c, high) == 1 && c.r[R_EAX] == 0x13579be0 &&
                  c.r[R_ESP] == STACK_TOP - 1020,
              "high heap thunk executes and preserves import stack discipline");
        check(recomp_run_thunk(&c, HEAP_LIMIT + 256) == 0, "reserved gap is not executable heap");
    }
    mem_shutdown();
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
