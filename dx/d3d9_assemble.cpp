#include "d3d9_assemble.h"
#include "d3d9_shader.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>

namespace d9sh {
namespace {
std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
    return std::string(s.substr(a, b - a));
}
bool number(const std::string &s, uint32_t &n) {
    if (s.empty())
        return false;
    n = 0;
    for (char c : s) {
        if (c < '0' || c > '9' || n > 2047)
            return false;
        n = n * 10 + unsigned(c - '0');
    }
    return n <= 2047;
}
uint32_t reg_bits(uint32_t type) {
    return ((type & 7) << 28) | ((type & 0x18) << 8);
}
// Parameter tokens carry bit 31, register type/index, then mask or swizzle.
// Relative constants add the address token only in shader model 2 and later.
bool operand(std::string s, bool dest, unsigned major, bool pixel, bool sat,
             std::vector<uint32_t> &out, bool declaration = false) {
    s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); }),
            s.end());
    uint32_t mod = 0, type = 0, index = 0, component = 0;
    bool relative = false;
    if (!dest && !s.empty() && s[0] == '-') {
        mod = 1;
        s.erase(0, 1);
    }
    std::string suffix;
    size_t close = s.find(']');
    size_t dot = s.find('.', close == std::string::npos ? 0 : close + 1);
    if (dot != std::string::npos) {
        suffix = s.substr(dot + 1);
        s.resize(dot);
    }
    if (s == "oPos")
        type = R_RASTOUT;
    else if (s == "oFog") {
        type = R_RASTOUT;
        index = 1;
    } else if (s == "oPts") {
        type = R_RASTOUT;
        index = 2;
    } else if (s.size() > 2 && s.substr(0, 2) == "oD") {
        type = R_ATTROUT;
        if (!number(s.substr(2), index) || index > 1)
            return false;
    } else if (s.size() > 2 && s.substr(0, 2) == "oT") {
        type = R_TEXCRDOUT;
        if (!number(s.substr(2), index) || index > 7)
            return false;
    } else if (pixel && s.size() > 2 && s.substr(0, 2) == "oC") {
        type = R_COLOROUT;
        if (!number(s.substr(2), index) || index > 3)
            return false;
    } else if (pixel && s == "oDepth") {
        type = R_DEPTHOUT;
    } else if (s.size() > 2 && s.substr(0, 2) == "c[") {
        type = R_CONST;
        if (s.back() != ']')
            return false;
        std::string expr = s.substr(2, s.size() - 3);
        if (expr.empty())
            return false;
        while (!expr.empty()) {
            size_t plus = expr.find('+');
            std::string term = expr.substr(0, plus);
            if (term.size() == 4 && term.substr(0, 3) == "a0.") {
                auto pos = std::string("xyzw").find(term[3]);
                if (relative || pixel || pos == std::string::npos || (major == 1 && pos != 0))
                    return false;
                component = uint32_t(pos);
                relative = true;
            } else {
                uint32_t offset;
                if (!number(term, offset) || index + offset > 2047)
                    return false;
                index += offset;
            }
            if (plus == std::string::npos)
                break;
            expr.erase(0, plus + 1);
            if (expr.empty())
                return false;
        }
    } else {
        if (s.size() < 2)
            return false;
        switch (s[0]) {
        case 'r':
            type = R_TEMP;
            break;
        case 'v':
            type = R_INPUT;
            break;
        case 'c':
            type = R_CONST;
            break;
        case 'a':
            if (pixel)
                return false;
            type = R_ADDR;
            break;
        case 't':
            if (!pixel)
                return false;
            type = R_ADDR;
            break;
        case 's':
            if (!pixel)
                return false;
            type = R_SAMPLER;
            break;
        default:
            return false;
        }
        if (!number(s.substr(1), index))
            return false;
    }
    if ((type == R_TEMP && index >= (major == 1 ? 12u : 32u)) ||
        (type == R_INPUT && index >= (pixel ? 2u : 16u)) ||
        (type == R_CONST && index >= (pixel ? 32u : 256u)) ||
        (type == R_ADDR && index >= (pixel ? 8u : 1u)) || (type == R_SAMPLER && index >= 16))
        return false;
    if (dest &&
        (type == R_CONST || relative ||
         (!declaration && (type == R_INPUT || type == R_SAMPLER || (pixel && type == R_ADDR)))))
        return false;
    if (!dest && (type == R_RASTOUT || type == R_ATTROUT || type == R_TEXCRDOUT ||
                  type == R_COLOROUT || type == R_DEPTHOUT))
        return false;
    uint32_t mask = dest ? 15 : 0xe4;
    if (!suffix.empty()) {
        if (suffix.size() > 4)
            return false;
        mask = 0;
        unsigned previous = 0;
        for (unsigned i = 0; i < suffix.size(); ++i) {
            size_t c = std::string("xyzw").find(suffix[i]);
            if (c == std::string::npos)
                c = std::string("rgba").find(suffix[i]);
            if (c == std::string::npos)
                return false;
            if (dest) {
                if (i && c <= previous)
                    return false;
                mask |= 1u << c;
                previous = unsigned(c);
            } else
                mask |= uint32_t(c) << (2 * i);
        }
        if (!dest) {
            uint32_t last = (mask >> (2 * (suffix.size() - 1))) & 3;
            for (unsigned i = unsigned(suffix.size()); i < 4; ++i)
                mask |= last << (2 * i);
        }
    }
    out.push_back(0x80000000u | reg_bits(type) | index | (mask << 16) |
                  (dest ? (sat ? 1u << 20 : 0) : mod << 24) | (relative ? 1u << 13 : 0));
    if (relative && major >= 2)
        out.push_back(0x80000000u | reg_bits(R_ADDR) | ((component * 0x55) << 16));
    return true;
}
struct Operation {
    uint32_t op;
    unsigned sources;
};
const std::map<std::string, Operation> ops = {
    {"mov", {OP_MOV, 1}},       {"mova", {OP_MOVA, 1}}, {"add", {OP_ADD, 2}},
    {"sub", {OP_SUB, 2}},       {"mul", {OP_MUL, 2}},   {"mad", {OP_MAD, 3}},
    {"dp3", {OP_DP3, 2}},       {"dp4", {OP_DP4, 2}},   {"rcp", {OP_RCP, 1}},
    {"rsq", {OP_RSQ, 1}},       {"dst", {OP_DST, 2}},   {"min", {OP_MIN, 2}},
    {"max", {OP_MAX, 2}},       {"slt", {OP_SLT, 2}},   {"sge", {OP_SGE, 2}},
    {"exp", {OP_EXP, 1}},       {"expp", {OP_EXPP, 1}}, {"log", {OP_LOG, 1}},
    {"logp", {OP_LOGP, 1}},     {"lit", {OP_LIT, 1}},   {"frc", {OP_FRC, 1}},
    {"lrp", {OP_LRP, 3}},       {"abs", {OP_ABS, 1}},   {"nrm", {OP_NRM, 1}},
    {"pow", {OP_POW, 2}},       {"crs", {OP_CRS, 2}},   {"m4x4", {OP_M4X4, 2}},
    {"m4x3", {OP_M4X3, 2}},     {"m3x4", {OP_M3X4, 2}}, {"m3x3", {OP_M3X3, 2}},
    {"m3x2", {OP_M3X2, 2}},     {"texld", {OP_TEX, 2}}, {"cmp", {OP_CMP, 3}},
    {"dp2add", {OP_DP2ADD, 3}},
};
} // namespace
bool assemble(std::string_view source, std::vector<uint32_t> &words, std::string &error) {
    words.clear();
    error.clear();
    unsigned major = 0, line = 0;
    bool pixel = false;
    auto fail = [&](std::string why) {
        words.clear();
        error = "line " + std::to_string(line) + ": " + why + "\n";
        return false;
    };
    if (source.size() > 1024 * 1024 || source.find('\0') != std::string_view::npos)
        return fail("invalid source length or embedded NUL");
    while (!source.empty()) {
        ++line;
        size_t end = source.find('\n');
        std::string s = trim(source.substr(0, end));
        source = end == std::string_view::npos ? std::string_view{} : source.substr(end + 1);
        size_t comment = std::min(s.find(';'), s.find("//"));
        if (comment != std::string::npos)
            s = trim(std::string_view(s).substr(0, comment));
        if (s.empty())
            continue;
        if (!major) {
            if (s == "vs_1_1" || s == "vs.1.1") {
                major = 1;
                words.push_back(0xfffe0101);
            } else if (s == "vs_2_0" || s == "vs.2.0") {
                major = 2;
                words.push_back(0xfffe0200);
            } else if (s == "ps_2_0" || s == "ps.2.0") {
                major = 2;
                pixel = true;
                words.push_back(0xffff0200);
            } else
                return fail("unsupported shader version: " + s);
            continue;
        }
        size_t space = s.find_first_of(" \t");
        std::string name = s.substr(0, space),
                    args = space == std::string::npos ? "" : trim(s.substr(space));
        if (name == "nop" && args.empty()) {
            words.push_back(OP_NOP);
            continue;
        }
        bool sat = name.size() > 4 && name.substr(name.size() - 4) == "_sat";
        if (sat)
            name.resize(name.size() - 4);
        std::vector<std::string> operands;
        while (!args.empty()) {
            size_t comma = args.find(',');
            operands.push_back(trim(args.substr(0, comma)));
            if (comma == std::string::npos)
                break;
            args = args.substr(comma + 1);
            if (trim(args).empty())
                return fail("trailing comma");
        }
        std::vector<uint32_t> tokens;
        uint32_t op = 0;
        if (name == "dcl" || name.substr(0, 4) == "dcl_") {
            if (major < 2 || sat || operands.size() != 1)
                return fail("invalid declaration");
            static const std::map<std::string, uint32_t> usages = {
                {"position", 0}, {"blendweight", 1}, {"blendindices", 2},
                {"normal", 3},   {"psize", 4},       {"texcoord", 5},
                {"tangent", 6},  {"binormal", 7},    {"color", 10}};
            if (pixel) {
                uint32_t texture_type = name == "dcl_2d"       ? 2
                                        : name == "dcl_cube"   ? 3
                                        : name == "dcl_volume" ? 4
                                                               : 0;
                if (name != "dcl" && !texture_type)
                    return fail("unsupported pixel declaration");
                tokens.push_back(0x80000000u | (texture_type << 27));
                if (!operand(operands[0], true, major, pixel, false, tokens, true))
                    return fail("invalid declaration register");
                uint32_t type = ((tokens.back() >> 28) & 7) | ((tokens.back() & 0x1800) >> 8);
                if (texture_type ? type != R_SAMPLER : (type != R_INPUT && type != R_ADDR))
                    return fail("invalid declaration register type");
            } else {
                if (name == "dcl")
                    return fail("vertex declaration requires a semantic");
                std::string semantic = name.substr(4);
                uint32_t usage_index = 0;
                size_t digit = semantic.find_first_of("0123456789");
                if (digit != std::string::npos) {
                    if (!number(semantic.substr(digit), usage_index) || usage_index > 15)
                        return fail("invalid semantic index");
                    semantic.resize(digit);
                }
                auto usage = usages.find(semantic);
                if (usage == usages.end())
                    return fail("unsupported declaration: " + name);
                tokens.push_back(0x80000000u | usage->second | (usage_index << 16));
                // Declaration registers are input destinations, unlike instruction destinations.
                if (!operand(operands[0], true, major, pixel, false, tokens, true) ||
                    ((tokens.back() >> 28) & 7) != R_INPUT)
                    return fail("invalid declaration register");
            }
            op = OP_DCL;
        } else if (name == "def") {
            if (sat || operands.size() != 5 || operands[0].empty() || operands[0][0] != 'c')
                return fail("invalid def");
            uint32_t index;
            if (!number(operands[0].substr(1), index) || index >= (pixel ? 32u : 256u))
                return fail("invalid constant");
            tokens.push_back(0x800f0000u | reg_bits(R_CONST) | index);
            for (unsigned i = 1; i <= 4; ++i) {
                char *endp = nullptr;
                if (!operands[i].empty() && operands[i].back() == 'f')
                    operands[i].pop_back();
                float value = std::strtof(operands[i].c_str(), &endp);
                if (!endp || endp == operands[i].c_str() || *endp)
                    return fail("invalid constant value");
                uint32_t bits;
                std::memcpy(&bits, &value, 4);
                tokens.push_back(bits);
            }
            op = OP_DEF;
        } else {
            auto found = ops.find(name);
            if (found == ops.end() || operands.size() != 1 + found->second.sources)
                return fail("unsupported instruction or operand count: " + name);
            op = found->second.op;
            if (major == 1 && op == OP_MOVA)
                return fail("mova requires shader model 2");
            for (unsigned i = 0; i < operands.size(); ++i) {
                std::string text = operands[i];
                // DX9 encodes vertex sub as add with the second source negated.
                if (!pixel && op == OP_SUB && i == 2)
                    text = !text.empty() && text[0] == '-' ? text.substr(1) : "-" + text;
                if (!operand(text, i == 0, major, pixel, sat, tokens))
                    return fail("invalid operand: " + text);
            }
            if (!pixel && op == OP_SUB)
                op = OP_ADD;
        }
        words.push_back(op | (major >= 2 ? uint32_t(tokens.size()) << 24 : 0));
        words.insert(words.end(), tokens.begin(), tokens.end());
    }
    if (!major)
        return fail("missing shader version");
    words.push_back(OP_END);
    return true;
}
} // namespace d9sh
