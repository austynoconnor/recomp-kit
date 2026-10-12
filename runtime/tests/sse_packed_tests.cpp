// Execute fixtures emitted by the production translator, without game inputs.
#include "../memory.h"
#include "game_config.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <initializer_list>
extern "C" {
#include "packed_sse_fixture.h"
}

using Fixture = void (*)(X86 *);
struct SingleCase {
    const char *name;
    Fixture forms[3];
    float normal[4], alias[4];
};
struct DoubleCase {
    const char *name;
    Fixture forms[3];
    double normal[2], alias[2];
};

static X86 initial(uint32_t source) {
    X86 c{};
    for (unsigned i = 0; i < 8; ++i)
        c.r[i] = 0x12345600 + i;
    c.r[R_ESI] = source - 16;
    c.r[R_ESP] = STACK_TOP - 4096;
    c.eflags_cf = c.eflags_zf = c.eflags_sf = c.eflags_of = 1;
    c.eflags_pf = c.eflags_af = c.eflags_df = 1;
    c.eflags_misc = 0x202;
    std::memset(c.xmm, 0xa5, sizeof c.xmm);
    return c;
}

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
    const uint32_t source = RECOMP_EXTRA_HEAP_SIZE ? RECOMP_EXTRA_HEAP_BASE + 256 : 0x00310000;
    const float a[4] = {2, -3, 0.5f, 10}, b[4] = {4, 2, -8, -0.5f};
    const double ad[2] = {2, -3}, bd[2] = {4, 2};
    const SingleCase singles[] = {
        {"ADDPS",
         {packed_addps_reg, packed_addps_mem, packed_addps_alias},
         {6, -1, -7.5f, 9.5f},
         {4, -6, 1, 20}},
        {"SUBPS",
         {packed_subps_reg, packed_subps_mem, packed_subps_alias},
         {-2, -5, 8.5f, 10.5f},
         {0, 0, 0, 0}},
        {"MULPS",
         {packed_mulps_reg, packed_mulps_mem, packed_mulps_alias},
         {8, -6, -4, -5},
         {4, 9, 0.25f, 100}},
        {"DIVPS",
         {packed_divps_reg, packed_divps_mem, packed_divps_alias},
         {0.5f, -1.5f, -0.0625f, -20},
         {1, 1, 1, 1}},
    };
    const DoubleCase doubles[] = {
        {"ADDPD", {packed_addpd_reg, packed_addpd_mem, packed_addpd_alias}, {6, -1}, {4, -6}},
        {"SUBPD", {packed_subpd_reg, packed_subpd_mem, packed_subpd_alias}, {-2, -5}, {0, 0}},
        {"MULPD", {packed_mulpd_reg, packed_mulpd_mem, packed_mulpd_alias}, {8, -6}, {4, 9}},
        {"DIVPD", {packed_divpd_reg, packed_divpd_mem, packed_divpd_alias}, {0.5, -1.5}, {1, 1}},
    };
    auto state = [&](const X86 &c, const X86 &before) {
        check(std::memcmp(c.r, before.r, sizeof c.r) == 0, "packed arithmetic preserves GPRs");
        check(std::memcmp(&c.eflags_cf, &before.eflags_cf, 8 * sizeof(uint32_t)) == 0,
              "packed arithmetic preserves EFLAGS");
        check(std::memcmp(c.xmm + 1, before.xmm + 1, 7 * sizeof c.xmm[0]) == 0,
              "packed arithmetic preserves other XMM registers");
    };
    for (const auto &test : singles)
        for (unsigned mode = 0; mode < 3; ++mode) {
            X86 c = initial(source);
            std::memcpy(c.xmm[0], a, 16);
            std::memcpy(c.xmm[1], b, 16);
            std::memcpy(g_mem + source, b, 16);
            X86 before = c;
            test.forms[mode](&c);
            check(std::memcmp(c.xmm[0], mode == 2 ? test.alias : test.normal, 16) == 0, test.name);
            state(c, before);
            check(std::memcmp(g_mem + source, b, 16) == 0, "single memory source unchanged");
        }
    for (const auto &test : doubles)
        for (unsigned mode = 0; mode < 3; ++mode) {
            X86 c = initial(source);
            std::memcpy(c.xmm[0], ad, 16);
            std::memcpy(c.xmm[1], bd, 16);
            std::memcpy(g_mem + source, bd, 16);
            X86 before = c;
            test.forms[mode](&c);
            check(std::memcmp(c.xmm[0], mode == 2 ? test.alias : test.normal, 16) == 0, test.name);
            state(c, before);
            check(std::memcmp(g_mem + source, bd, 16) == 0, "double memory source unchanged");
        }

    // Separate instructions must round separately, rather than becoming FMA.
    X86 c{};
    const float near_one[4] = {1.00000011920928955078125f, 1.00000011920928955078125f,
                               1.00000011920928955078125f, 1.00000011920928955078125f};
    const float below_one[4] = {0.99999988079071044921875f, 0.99999988079071044921875f,
                                0.99999988079071044921875f, 0.99999988079071044921875f};
    const float ones[4] = {1, 1, 1, 1};
    std::memcpy(c.xmm[0], near_one, 16);
    std::memcpy(c.xmm[1], below_one, 16);
    std::memcpy(c.xmm[2], ones, 16);
    packed_rounding(&c);
    check(c.xmm[0][0] == 0 && c.xmm[0][1] == 0 && c.xmm[0][2] == 0 && c.xmm[0][3] == 0,
          "multiply/subtract retains separate single-precision rounding");
    const float edge[4] = {-0.0f, std::numeric_limits<float>::infinity(), 1e30f,
                           std::numeric_limits<float>::quiet_NaN()};
    const float factors[4] = {2, 0, 1e30f, 1};
    std::memcpy(c.xmm[0], edge, 16);
    std::memcpy(c.xmm[1], factors, 16);
    packed_mulps_reg(&c);
    float result[4];
    std::memcpy(result, c.xmm[0], 16);
    check(c.xmm[0][0] == 0x80000000, "multiplication preserves negative zero");
    check(std::isnan(result[1]), "invalid masked multiplication returns NaN");
    check(std::isinf(result[2]) && result[2] > 0, "single-precision overflow returns infinity");
    check(std::isnan(result[3]), "NaN classification survives packed arithmetic");

    // Same shuffle/multiply/add sequence as the observed transform, with an
    // independent golden vector and destinations aliasing either source.
    const uint32_t input = 0x00312000, matrix = input + 16;
    const float rows[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    const float vector[4] = {2, -1, 0.5f, 1}, expected[4] = {14.5f, 17, 19.5f, 22};
    for (uint32_t output : {input + 96, input, matrix}) {
        std::memcpy(g_mem + input, vector, 16);
        std::memcpy(g_mem + matrix, rows, 64);
        c = initial(source);
        c.r[R_ESI] = input;
        c.r[R_EDI] = matrix;
        c.r[R_EBX] = output;
        X86 before = c;
        packed_matrix(&c);
        check(std::memcmp(g_mem + output, expected, 16) == 0,
              "transform golden vector / source aliasing");
        check(std::memcmp(c.r, before.r, sizeof c.r) == 0, "transform preserves GPRs");
        check(std::memcmp(&c.eflags_cf, &before.eflags_cf, 8 * sizeof(uint32_t)) == 0,
              "transform preserves EFLAGS");
    }
    c = {};
    c.r[R_EAX] = 1;
    recomp_cpuid(&c);
    check((c.r[R_EDX] & ((1u << 25) | (1u << 26))) == 0 && (c.r[R_ECX] & (1u << 28)) == 0,
          "partial SIMD support does not advertise complete SSE/AVX");
    mem_shutdown();
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
