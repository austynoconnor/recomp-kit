#include "../d3d9_assemble.h"
#include "../d3d9_shader.h"
#include <cstring>
#include <cstdio>
#include <initializer_list>

int main() {
    unsigned failures = 0, checks = 0;
    auto fixture = [&](const char *source, std::initializer_list<uint32_t> expected) {
        std::vector<uint32_t> words;
        std::string error;
        ++checks;
        if (!d9sh::assemble(source, words, error) || words != std::vector<uint32_t>(expected)) {
            ++failures;
            std::printf("fixture failed: %s\n%s", source, error.c_str());
            for (uint32_t w : words)
                std::printf("%08x ", w);
            std::puts("");
        } else {
            ++checks;
            std::vector<uint8_t> bytes(words.size() * 4);
            std::memcpy(bytes.data(), words.data(), bytes.size());
            const d9sh::Program &decoded = d9sh::program_for(bytes);
            if (!decoded.ok || decoded.pixel != (words[0] >> 16 == 0xffff) ||
                decoded.major != ((words[0] >> 8) & 255)) {
                ++failures;
                std::printf("decoder roundtrip failed: %s\n", decoded.why.c_str());
            }
        }
    };
    fixture("vs_1_1\nmov oPos, v0\n", {0xfffe0101, 1, 0xc00f0000, 0x90e40000, 0xffff});
    fixture("; comment\r\nvs.2.0\r\ndcl_position v0 // position\nmov oPos, v0\n",
            {0xfffe0200, 0x0200001f, 0x80000000, 0x900f0000, 0x02000001, 0xc00f0000, 0x90e40000,
             0xffff});
    fixture("vs_1_1\nm4x4 oPos, v0, c[a0.x + 4]\n",
            {0xfffe0101, 20, 0xc00f0000, 0x90e40000, 0xa0e42004, 0xffff});
    fixture("vs_2_0\nm4x4 oPos, v0, c[4 + a0.x]\n",
            {0xfffe0200, 0x04000014, 0xc00f0000, 0x90e40000, 0xa0e42004, 0xb0000000, 0xffff});
    fixture("vs_1_1\nsub r0.xy, r1.y, -c2\n",
            {0xfffe0101, 2, 0x80030000, 0x80550001, 0xa0e40002, 0xffff});
    fixture("vs_2_0\nmov_sat r0.xyz, -v0.xy\n",
            {0xfffe0200, 0x02000001, 0x80170000, 0x91540000, 0xffff});
    fixture("vs_1_1\ndef c0, 1, -2, 0, 0.5\nmov oPos, c0\n",
            {0xfffe0101, 81, 0xa00f0000, 0x3f800000, 0xc0000000, 0, 0x3f000000, 1, 0xc00f0000,
             0xa0e40000, 0xffff});
    fixture("vs_2_0\nm4x3 r2.xyz,v0,c[0 + 10]\n",
            {0xfffe0200, 0x03000015, 0x80070002, 0x90e40000, 0xa0e4000a, 0xffff});
    fixture("ps_2_0\ndcl t0.xy\ndcl_2d s0\ntexld r0,t0,s0\nmov oC0,r0\n",
            {0xffff0200, 0x0200001f, 0x80000000, 0xb0030000, 0x0200001f, 0x90000000, 0xa00f0800,
             0x03000042, 0x800f0000, 0xb0e40000, 0xa0e40800, 0x02000001, 0x800f0800, 0x80e40000,
             0xffff});
    fixture("ps_2_0\nmov r0.r,v0.g\n", {0xffff0200, 0x02000001, 0x80010000, 0x90550000, 0xffff});
    for (const char *source :
         {"", "ps_3_0\nmov r0,v0", "vs_1_1\nmov c0,v0", "vs_1_1\nmova a0.x,r0.x",
          "vs_2_0\nmov r0,v16", "vs_2_0\nmov r32,v0", "vs_1_1\nmov r0,c[a0.y]",
          "vs_2_0\nmov r0,c[a0.x-4]", "vs_2_0\nmov r0,v0,", "vs_2_0\nmadeup r0,v0",
          "vs_1_1\nmov r0.xx,v0"}) {
        std::vector<uint32_t> words = {123};
        std::string error;
        ++checks;
        if (d9sh::assemble(source, words, error) || !words.empty() || error.empty()) {
            ++failures;
            std::printf("invalid source accepted: %s\n", source);
        }
    }
    std::printf("%u shader assembly checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
