// d3d9_ffp.cpp - the fixed-function pipeline as vs_2_0 / ps_2_0 bytecode.
// See d3d9_ffp.h. Written for the kit from the Direct3D documentation of the
// fixed-function pipeline; no driver or wrapper code was copied.
#include "d3d9_ffp.h"
#include "d3d9_shader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace {

// ---------------------------------------------------------------------------
// Bytecode writing
// ---------------------------------------------------------------------------
enum : uint32_t {
    T_TEMP = 0,
    T_INPUT = 1,
    T_CONST = 2,
    T_TEXTURE = 3, // ps: tN
    T_RASTOUT = 4, // vs: oPos (0), oFog (1)
    T_ATTROUT = 5, // vs: oD0, oD1
    T_TEXCRDOUT = 6,
    T_COLOROUT = 8,
    T_SAMPLER = 10,
};
enum : uint32_t {
    O_MOV = 1,
    O_ADD = 2,
    O_MAD = 4,
    O_MUL = 5,
    O_RCP = 6,
    O_RSQ = 7,
    O_DP3 = 8,
    O_DP4 = 9,
    O_MAX = 11,
    O_SLT = 12,
    O_EXP = 14,
    O_LRP = 18,
    O_M4X4 = 20,
    O_M3X3 = 23,
    O_DCL = 31,
    O_POW = 32,
    O_NRM = 36,
    O_TEX = 66,
    O_DEF = 81,
};
const uint32_t XYZW = 0xe4, XXXX = 0x00, YYYY = 0x55, ZZZZ = 0xaa, WWWW = 0xff;
const uint32_t M_X = 1, M_Y = 2, M_Z = 4, M_W = 8, M_XYZ = 7, M_ALL = 15;
const uint32_t NEG = 1;

uint32_t reg(uint32_t type, uint32_t index) {
    return 0x80000000u | (index & 0x7ff) | ((type & 7) << 28) | ((type & 0x18) << 8);
}
uint32_t dst(uint32_t type, uint32_t index, uint32_t mask = M_ALL, bool sat = false) {
    return reg(type, index) | mask << 16 | (sat ? 1u << 20 : 0u);
}
uint32_t src(uint32_t type, uint32_t index, uint32_t swz = XYZW, uint32_t mod = 0) {
    return reg(type, index) | swz << 16 | mod << 24;
}
// The same source with a different swizzle or a negation.
uint32_t swz(uint32_t s, uint32_t sw) {
    return (s & ~(0xffu << 16)) | sw << 16;
}
uint32_t neg(uint32_t s) {
    return (s & ~(0xfu << 24)) | NEG << 24;
}
// Replicates component k (0..3).
uint32_t rep(uint32_t s, int k) {
    static const uint32_t r[4] = {XXXX, YYYY, ZZZZ, WWWW};
    return swz(s, r[k & 3]);
}
uint32_t fbits(float f) {
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}

struct Asm {
    std::vector<uint32_t> t;
    explicit Asm(uint32_t version) {
        t.push_back(version);
    }
    void op(uint32_t code, std::initializer_list<uint32_t> args, uint32_t ctrl = 0) {
        t.push_back(code | (ctrl & 0xff) << 16 | (uint32_t)args.size() << 24);
        for (uint32_t a : args)
            t.push_back(a);
    }
    void def(uint32_t index, float a, float b, float c, float d) {
        op(O_DEF, {dst(T_CONST, index), fbits(a), fbits(b), fbits(c), fbits(d)});
    }
    void dcl(uint32_t token, uint32_t reg_dst) {
        op(O_DCL, {0x80000000u | token, reg_dst});
    }
    std::vector<uint8_t> bytes() {
        t.push_back(0x0000ffffu);
        std::vector<uint8_t> b(t.size() * 4);
        memcpy(b.data(), t.data(), b.size());
        return b;
    }
};

// ---------------------------------------------------------------------------
// What the vertices carry
// ---------------------------------------------------------------------------
struct Inputs {
    bool position = false, positiont = false, normal = false, color[2] = {false, false};
    uint8_t tex_dims[8] = {}; // components of TEXCOORDn, 0 when absent
};

uint32_t decl_type_dims(uint32_t type) {
    switch (type) {
    case 0: // FLOAT1
        return 1;
    case 1:  // FLOAT2
    case 6:  // SHORT2
    case 9:  // SHORT2N
    case 11: // USHORT2N
    case 15: // FLOAT16_2
        return 2;
    case 2:  // FLOAT3
    case 13: // UDEC3
    case 14: // DEC3N
        return 3;
    default:
        return 4;
    }
}

Inputs inputs_of(const std::vector<uint8_t> &decl) {
    Inputs in;
    for (size_t i = 0; i + 8 <= decl.size(); i += 8) {
        uint32_t stream = decl[i] | decl[i + 1] << 8;
        if (stream == 0xff)
            break;
        uint32_t type = decl[i + 4], usage = decl[i + 6], index = decl[i + 7];
        switch (usage) {
        case 0: // POSITION
            if (index == 0)
                in.position = true;
            break;
        case 9: // POSITIONT
            in.positiont = true;
            break;
        case 3: // NORMAL
            if (index == 0)
                in.normal = true;
            break;
        case 10: // COLOR
            if (index < 2)
                in.color[index] = true;
            break;
        case 5: // TEXCOORD
            if (index < 8)
                in.tex_dims[index] = (uint8_t)decl_type_dims(type);
            break;
        default:
            break;
        }
    }
    return in;
}

// ---------------------------------------------------------------------------
// Matrices: D3DMATRIX, row-major, row vectors (v' = v * M).
// ---------------------------------------------------------------------------
struct Mat {
    float m[16];
};
Mat mat(const float *p) {
    Mat r;
    memcpy(r.m, p, sizeof r.m);
    return r;
}
Mat mul(const Mat &a, const Mat &b) {
    Mat r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k)
                s += a.m[i * 4 + k] * b.m[k * 4 + j];
            r.m[i * 4 + j] = s;
        }
    return r;
}
// Writes the columns of `m` as four constant registers, so that dp4 / m4x4
// against them is v * M.
void put_columns(float (*c)[4], uint32_t at, const Mat &m) {
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i)
            c[at + j][i] = m.m[i * 4 + j];
}
// The normal matrix: the inverse transpose of the upper 3x3, written as the
// three columns m3x3 needs (which are the rows of the inverse).
void put_normal_matrix(float (*c)[4], uint32_t at, const Mat &m) {
    const float *a = m.m;
    float a00 = a[0], a01 = a[1], a02 = a[2], a10 = a[4], a11 = a[5], a12 = a[6], a20 = a[8],
          a21 = a[9], a22 = a[10];
    float c00 = a11 * a22 - a12 * a21, c01 = a12 * a20 - a10 * a22, c02 = a10 * a21 - a11 * a20;
    float det = a00 * c00 + a01 * c01 + a02 * c02;
    float inv[9];
    if (std::fabs(det) < 1e-20f) {
        float id[9] = {a00, a01, a02, a10, a11, a12, a20, a21, a22};
        memcpy(inv, id, sizeof inv); // degenerate: fall back to the matrix itself
        for (int j = 0; j < 3; ++j)
            for (int i = 0; i < 3; ++i)
                c[at + j][i] = inv[i * 3 + j];
        return;
    }
    float d = 1.0f / det;
    inv[0] = c00 * d;
    inv[1] = (a02 * a21 - a01 * a22) * d;
    inv[2] = (a01 * a12 - a02 * a11) * d;
    inv[3] = c01 * d;
    inv[4] = (a00 * a22 - a02 * a20) * d;
    inv[5] = (a02 * a10 - a00 * a12) * d;
    inv[6] = c02 * d;
    inv[7] = (a01 * a20 - a00 * a21) * d;
    inv[8] = (a00 * a11 - a01 * a10) * d;
    // Column j of the inverse transpose is row j of the inverse.
    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i)
            c[at + j][i] = inv[j * 3 + i];
        c[at + j][3] = 0;
    }
}
void put_color(float *dst4, uint32_t argb) {
    dst4[0] = ((argb >> 16) & 0xff) / 255.0f;
    dst4[1] = ((argb >> 8) & 0xff) / 255.0f;
    dst4[2] = (argb & 0xff) / 255.0f;
    dst4[3] = (argb >> 24) / 255.0f;
}
float as_float(uint32_t b) {
    float f;
    memcpy(&f, &b, 4);
    return f;
}

} // namespace

// ---------------------------------------------------------------------------
// FVF codes
// ---------------------------------------------------------------------------
namespace {
struct Element {
    uint16_t offset;
    uint8_t type, usage, index;
};
std::vector<Element> fvf_elements(uint32_t fvf, uint32_t *stride) {
    std::vector<Element> e;
    uint32_t at = 0;
    auto add = [&](uint8_t type, uint8_t usage, uint8_t index, uint32_t bytes) {
        e.push_back({(uint16_t)at, type, usage, index});
        at += bytes;
    };
    switch (fvf & 0x400e) {
    case 0x0002: // XYZ
        add(2, 0, 0, 12);
        break;
    case 0x0004: // XYZRHW
        add(3, 9, 0, 16);
        break;
    case 0x4002: // XYZW
        add(3, 0, 0, 16);
        break;
    case 0x0006:
    case 0x0008:
    case 0x000a:
    case 0x000c:
    case 0x000e: { // XYZB1..B5: the blend weights are carried, not applied
        uint32_t betas = ((fvf & 0xe) - 4) / 2;
        add(2, 0, 0, 12);
        bool ubyte4 = (fvf & 0x1000) != 0; // LASTBETA_UBYTE4: the last beta is indices
        uint32_t weights = ubyte4 ? betas - 1 : betas;
        if (weights)
            add((uint8_t)(weights - 1), 1, 0, 4 * weights);
        if (ubyte4)
            add(5, 2, 0, 4);
        break;
    }
    default:
        *stride = 0;
        return {};
    }
    if (fvf & 0x10)
        add(2, 3, 0, 12); // NORMAL
    if (fvf & 0x20)
        add(0, 4, 0, 4); // PSIZE
    if (fvf & 0x40)
        add(4, 10, 0, 4); // DIFFUSE
    if (fvf & 0x80)
        add(4, 10, 1, 4); // SPECULAR
    uint32_t count = (fvf >> 8) & 0xf;
    for (uint32_t i = 0; i < count && i < 8; ++i) {
        // D3DFVF_TEXCOORDSIZEn: 0 two floats, 1 three, 2 four, 3 one.
        static const uint8_t dims[4] = {2, 3, 4, 1};
        uint8_t n = dims[(fvf >> (16 + 2 * i)) & 3];
        add((uint8_t)(n - 1), 5, (uint8_t)i, 4u * n);
    }
    *stride = at;
    return e;
}
} // namespace

std::vector<uint8_t> d9_fvf_declaration(uint32_t fvf) {
    uint32_t stride = 0;
    std::vector<Element> e = fvf_elements(fvf, &stride);
    std::vector<uint8_t> d;
    if (e.empty())
        return d;
    for (const Element &x : e) {
        uint8_t r[8] = {
            0,       0,      (uint8_t)(x.offset & 0xff), (uint8_t)(x.offset >> 8), x.type, 0,
            x.usage, x.index};
        d.insert(d.end(), r, r + 8);
    }
    static const uint8_t end[8] = {0xff, 0, 0, 0, 17, 0, 0, 0}; // D3DDECL_END
    d.insert(d.end(), end, end + 8);
    return d;
}

uint32_t d9_fvf_stride(uint32_t fvf) {
    uint32_t stride = 0;
    fvf_elements(fvf, &stride);
    return stride;
}

// ---------------------------------------------------------------------------
// The vertex program
// ---------------------------------------------------------------------------
namespace {
// Everything that changes the vertex program's code (not its constants).
// Zero-filled and compared as bytes, so it holds only bytes.
struct VsState {
    uint8_t positiont, normal, color0, color1;
    uint8_t tex_dims[8];
    uint8_t lighting, specular, local_viewer, nlights;
    uint8_t src_diffuse, src_ambient, src_specular, src_emissive; // 0 material, 1/2 colour
    uint8_t light_type[8];
    uint8_t fog; // 0 none, 1 exp, 2 exp2, 3 linear, 4 the specular alpha
    uint8_t range_fog;
    uint8_t tci_gen[8], tci_index[8], tt_count[8], tt_projected[8];
};

// Constant registers of the vertex program.
enum : uint32_t {
    VC_WVP = 0,
    VC_WV = 4,
    VC_NORMAL = 8,
    VC_MAT = 12, // diffuse, ambient, specular, emissive, power
    VC_AMBIENT = 17,
    VC_FOG = 18,
    VC_RHW = 19,
    VC_LIGHT = 20,  // seven per light
    VC_TEXMAT = 80, // four per stage
    VC_VIEW_DIR = 93,
    VC_ZERO_ONE = 94,
};
const uint32_t LIGHT_REGS = 7;

uint32_t vin(uint32_t i) {
    return src(T_INPUT, i);
}
uint32_t vc(uint32_t i, uint32_t sw = XYZW) {
    return src(T_CONST, i, sw);
}
uint32_t vr(uint32_t i, uint32_t sw = XYZW) {
    return src(T_TEMP, i, sw);
}
uint32_t vd(uint32_t i, uint32_t mask = M_ALL, bool sat = false) {
    return dst(T_TEMP, i, mask, sat);
}

// Input registers: v0 position, v1 normal, v2/v3 colours, v4 + n TEXCOORDn.
enum : uint32_t { VI_POS = 0, VI_NORMAL = 1, VI_COLOR0 = 2, VI_COLOR1 = 3, VI_TEX = 4 };

void emit_light(Asm &a, const VsState &s, uint32_t i, uint32_t zero, uint32_t one) {
    uint32_t b = VC_LIGHT + LIGHT_REGS * i;
    // Light registers: +0 diffuse, +1 specular, +2 ambient, +3 eye-space
    // position (w: range), +4 eye-space direction towards the light,
    // +5 attenuation 0, 1, 2, +6 spot cos(phi/2), 1/(cos(theta/2) -
    // cos(phi/2)), falloff. r5.xyz ends up the unit vector to the light and
    // r6.y the attenuation.
    if (s.light_type[i] == 3) { // directional
        a.op(O_MOV, {vd(5, M_XYZ), vc(b + 4)});
        a.op(O_MOV, {vd(6, M_Y), one});
    } else {
        a.op(O_ADD, {vd(5, M_XYZ), vc(b + 3), neg(vr(0))});
        a.op(O_DP3, {vd(5, M_W), vr(5), vr(5)});
        a.op(O_RSQ, {vd(6, M_W), vr(5, WWWW)});
        a.op(O_MUL, {vd(5, M_XYZ), vr(5), vr(6, WWWW)});
        a.op(O_MUL, {vd(6, M_X), vr(5, WWWW), vr(6, WWWW)}); // distance
        a.op(O_MAD, {vd(6, M_Y), vc(b + 5, ZZZZ), vr(5, WWWW), vc(b + 5, XXXX)});
        a.op(O_MAD, {vd(6, M_Y), vc(b + 5, YYYY), vr(6, XXXX), vr(6, YYYY)});
        a.op(O_RCP, {vd(6, M_Y), vr(6, YYYY)});
        a.op(O_SLT, {vd(6, M_Z), vr(6, XXXX), vc(b + 3, WWWW)}); // inside the range
        a.op(O_MUL, {vd(6, M_Y), vr(6, YYYY), vr(6, ZZZZ)});
        if (s.light_type[i] == 2) { // the spot cone
            a.op(O_DP3, {vd(7, M_X), vr(5), vc(b + 4)});
            a.op(O_ADD, {vd(7, M_X), vr(7, XXXX), neg(vc(b + 6, XXXX))});
            a.op(O_MUL, {vd(7, M_X, true), vr(7, XXXX), vc(b + 6, YYYY)});
            a.op(O_POW, {vd(7, M_X), vr(7, XXXX), vc(b + 6, ZZZZ)});
            a.op(O_MUL, {vd(6, M_Y), vr(6, YYYY), vr(7, XXXX)});
        }
    }
    a.op(O_DP3, {vd(5, M_W), vr(1), vr(5)});
    a.op(O_MAX, {vd(5, M_W), vr(5, WWWW), zero});
    a.op(O_MUL, {vd(5, M_W), vr(5, WWWW), vr(6, YYYY)});
    a.op(O_MAD, {vd(2), vr(5, WWWW), vc(b + 0), vr(2)});
    a.op(O_MAD, {vd(4), vr(6, YYYY), vc(b + 2), vr(4)});
    if (s.specular) {
        a.op(O_ADD, {vd(7, M_XYZ), vr(5), vr(10)});
        a.op(O_NRM, {vd(9), vr(7)});
        a.op(O_DP3, {vd(7, M_W), vr(1), vr(9)});
        a.op(O_MAX, {vd(7, M_W), vr(7, WWWW), zero});
        a.op(O_POW, {vd(7, M_W), vr(7, WWWW), vc(VC_MAT + 4, XXXX)});
        // No highlight on a side that faces away from the light.
        a.op(O_SLT, {vd(7, M_Z), zero, vr(5, WWWW)});
        a.op(O_MUL, {vd(7, M_W), vr(7, WWWW), vr(7, ZZZZ)});
        a.op(O_MUL, {vd(7, M_W), vr(7, WWWW), vr(6, YYYY)});
        a.op(O_MAD, {vd(3), vr(7, WWWW), vc(b + 1), vr(3)});
    }
}

void emit_fog(Asm &a, const VsState &s) {
    // The distance: eye-space depth, or the true range.
    if (s.range_fog) {
        a.op(O_DP3, {vd(5, M_X), vr(0), vr(0)});
        a.op(O_RSQ, {vd(5, M_Y), vr(5, XXXX)});
        a.op(O_MUL, {vd(5, M_X), vr(5, XXXX), vr(5, YYYY)});
    } else {
        a.op(O_MOV, {vd(5, M_X), vr(0, ZZZZ)});
    }
    if (s.fog == 3) { // (end - d) / (end - start)
        a.op(O_ADD, {vd(5, M_X), vc(VC_FOG, YYYY), neg(vr(5, XXXX))});
        a.op(O_MUL, {dst(T_RASTOUT, 1, M_X, true), vr(5, XXXX), vc(VC_FOG, WWWW)});
        return;
    }
    // exp(-(density d)) or exp(-(density d)^2), as a power of two.
    a.op(O_MUL, {vd(5, M_X), vr(5, XXXX), vc(VC_FOG, ZZZZ)});
    if (s.fog == 2)
        a.op(O_MUL, {vd(5, M_X), vr(5, XXXX), vr(5, XXXX)});
    a.op(O_MUL, {vd(5, M_X), vr(5, XXXX), neg(vc(VC_ZERO_ONE, WWWW))});
    a.op(O_EXP, {dst(T_RASTOUT, 1, M_X, true), vr(5, XXXX)});
}

void emit_texcoords(Asm &a, const VsState &s, uint32_t zero, uint32_t one) {
    for (uint32_t i = 0; i < 8; ++i) {
        uint32_t gen = s.tci_gen[i], idx = s.tci_index[i] & 7;
        uint32_t vec, dims;
        if (gen == 1 && !s.positiont) { // CAMERASPACENORMAL
            vec = vr(1);
            dims = 3;
        } else if (gen == 2 && !s.positiont) { // CAMERASPACEPOSITION
            vec = vr(0);
            dims = 3;
        } else if (gen == 3 && !s.positiont) { // CAMERASPACEREFLECTIONVECTOR
            // R = E - 2 (N . E) N, E the unit vector from the eye to the vertex.
            a.op(O_NRM, {vd(11), vr(0)});
            a.op(O_DP3, {vd(7, M_W), vr(1), vr(11)});
            a.op(O_ADD, {vd(7, M_W), vr(7, WWWW), vr(7, WWWW)});
            a.op(O_MAD, {vd(11, M_XYZ), neg(vr(7, WWWW)), vr(1), vr(11)});
            vec = vr(11);
            dims = 3;
        } else if (s.tex_dims[idx]) {
            vec = vin(VI_TEX + idx);
            dims = s.tex_dims[idx];
        } else {
            continue;
        }
        uint32_t oT = dst(T_TEXCRDOUT, i);
        if (!s.tt_count[i]) {
            a.op(O_MOV, {oT, vec});
            continue;
        }
        // Through the texture matrix: the coordinate gets a 1 after its last
        // component, so a 2D coordinate is (u, v, 1, 0) and the matrix's
        // third row translates it.
        a.op(O_MOV, {vd(5), zero});
        a.op(O_MOV, {vd(5, (1u << dims) - 1), vec});
        if (dims < 4)
            a.op(O_MOV, {vd(5, 1u << dims), one});
        a.op(O_M4X4, {vd(6), vr(5), vc(VC_TEXMAT + 4 * i)});
        if (s.tt_projected[i] && s.tt_count[i] >= 2 && s.tt_count[i] < 4)
            a.op(O_MOV, {vd(6, M_W), rep(vr(6), s.tt_count[i] - 1)});
        a.op(O_MOV, {oT, vr(6)});
    }
}

std::vector<uint8_t> build_vs(const VsState &s) {
    Asm a(0xfffe0200u);
    a.def(VC_ZERO_ONE, 0.0f, 1.0f, 0.5f, 1.44269504f);
    a.def(VC_VIEW_DIR, 0.0f, 0.0f, -1.0f, 0.0f);
    const uint32_t zero = vc(VC_ZERO_ONE, XXXX), one = vc(VC_ZERO_ONE, YYYY);
    a.dcl(s.positiont ? 9u : 0u, dst(T_INPUT, VI_POS));
    if (s.normal)
        a.dcl(3, dst(T_INPUT, VI_NORMAL));
    if (s.color0)
        a.dcl(10, dst(T_INPUT, VI_COLOR0));
    if (s.color1)
        a.dcl(10 | 1u << 16, dst(T_INPUT, VI_COLOR1));
    for (uint32_t i = 0; i < 8; ++i)
        if (s.tex_dims[i])
            a.dcl(5 | i << 16, dst(T_INPUT, VI_TEX + i));
    const uint32_t color0 = s.color0 ? vin(VI_COLOR0) : one;
    const uint32_t color1 = s.color1 ? vin(VI_COLOR1) : zero;

    if (s.positiont) {
        // Screen coordinates back to clip space: x and y through the
        // viewport, z as given, w = 1 / RHW for the rasterizer to divide out.
        a.op(O_MAD, {vd(5, M_X | M_Y), vin(VI_POS), vc(VC_RHW, 0x44), vc(VC_RHW, 0xee)});
        a.op(O_MOV, {vd(5, M_Z), vin(VI_POS)});
        a.op(O_RCP, {vd(5, M_W), swz(vin(VI_POS), WWWW)});
        a.op(O_MUL, {dst(T_RASTOUT, 0, M_XYZ), vr(5), vr(5, WWWW)});
        a.op(O_MOV, {dst(T_RASTOUT, 0, M_W), vr(5, WWWW)});
        a.op(O_MOV, {dst(T_ATTROUT, 0), color0});
        a.op(O_MOV, {dst(T_ATTROUT, 1), color1});
        if (s.fog == 4)
            a.op(O_MOV, {dst(T_RASTOUT, 1, M_X), swz(color1, WWWW)});
        emit_texcoords(a, s, zero, one);
        return a.bytes();
    }
    a.op(O_M4X4, {dst(T_RASTOUT, 0), vin(VI_POS), vc(VC_WVP)});
    a.op(O_M4X4, {vd(0), vin(VI_POS), vc(VC_WV)}); // eye-space position
    if (s.normal) {
        a.op(O_M3X3, {vd(1, M_XYZ), vin(VI_NORMAL), vc(VC_NORMAL)});
        a.op(O_NRM, {vd(6), vr(1)});
        a.op(O_MOV, {vd(1), vr(6)});
    } else {
        a.op(O_MOV, {vd(1), zero});
    }
    if (s.lighting) {
        auto source = [&](uint8_t which, uint32_t material) {
            return which == 1 ? vin(VI_COLOR0) : which == 2 ? vin(VI_COLOR1) : vc(material);
        };
        const uint32_t D = source(s.src_diffuse, VC_MAT + 0);
        const uint32_t A = source(s.src_ambient, VC_MAT + 1);
        const uint32_t S = source(s.src_specular, VC_MAT + 2);
        const uint32_t E = source(s.src_emissive, VC_MAT + 3);
        a.op(O_MOV, {vd(2), zero});
        a.op(O_MOV, {vd(3), zero});
        a.op(O_MOV, {vd(4), vc(VC_AMBIENT)});
        if (s.specular) { // V: from the vertex towards the eye
            if (s.local_viewer)
                a.op(O_NRM, {vd(10), neg(vr(0))});
            else
                a.op(O_MOV, {vd(10), vc(VC_VIEW_DIR)});
        }
        for (uint32_t i = 0; i < s.nlights; ++i)
            emit_light(a, s, i, zero, one);
        a.op(O_MOV, {vd(8), E});
        a.op(O_MAD, {vd(8, M_XYZ), A, vr(4), vr(8)});
        a.op(O_MAD, {vd(8, M_XYZ), D, vr(2), vr(8)});
        a.op(O_MOV, {vd(8, M_W), swz(D, WWWW)});
        a.op(O_MOV, {dst(T_ATTROUT, 0, M_ALL, true), vr(8)});
        if (s.specular) {
            a.op(O_MUL, {dst(T_ATTROUT, 1, M_XYZ, true), S, vr(3)});
            a.op(O_MOV, {dst(T_ATTROUT, 1, M_W), zero});
        } else {
            a.op(O_MOV, {dst(T_ATTROUT, 1), zero});
        }
    } else {
        a.op(O_MOV, {dst(T_ATTROUT, 0), color0});
        a.op(O_MOV, {dst(T_ATTROUT, 1), color1});
    }
    if (s.fog >= 1 && s.fog <= 3)
        emit_fog(a, s);
    emit_texcoords(a, s, zero, one);
    return a.bytes();
}
} // namespace

// ---------------------------------------------------------------------------
// The pixel program: the texture stage cascade
// ---------------------------------------------------------------------------
namespace {
struct PsStage {
    uint8_t colorop, carg1, carg2, carg0, alphaop, aarg1, aarg2, aarg0, result;
    uint8_t sample; // 0 nothing bound, 1 a 2D texture, 2 a cube
    uint8_t projected;
};
struct PsState {
    uint8_t nstages, specular;
    PsStage stage[8];
};

// Constant registers of the pixel program.
enum : uint32_t { PC_TFACTOR = 0, PC_STAGE = 1, PC_LITERALS = 30, PC_ZERO = 31 };
// Temporaries: r0 current, r1 temp, r2 this stage's texel, r3-r5 its
// arguments, r6 the stage result, r7-r8 scratch.
enum : uint32_t { R_CURRENT = 0, R_TEMPREG = 1, R_TEXEL = 2, R_RESULT = 6 };

// D3DTA_* as a source token. A complemented argument is computed into
// `scratch` first, since 1 - x is not a ps_2_0 source modifier.
uint32_t stage_arg(Asm &a, uint32_t ta, uint32_t stage, bool sampled, uint32_t scratch) {
    uint32_t base;
    switch (ta & 0xf) {
    case 0: // DIFFUSE
        base = src(T_INPUT, 0);
        break;
    case 1: // CURRENT
        base = src(T_TEMP, R_CURRENT);
        break;
    case 2: // TEXTURE; a stage with nothing bound reads opaque white
        base = sampled ? src(T_TEMP, R_TEXEL) : src(T_CONST, PC_LITERALS, XXXX);
        break;
    case 3: // TFACTOR
        base = src(T_CONST, PC_TFACTOR);
        break;
    case 4: // SPECULAR
        base = src(T_INPUT, 1);
        break;
    case 5: // TEMP
        base = src(T_TEMP, R_TEMPREG);
        break;
    case 6: // CONSTANT
        base = src(T_CONST, PC_STAGE + stage);
        break;
    default:
        base = src(T_CONST, PC_ZERO);
        break;
    }
    if (ta & 0x20) // ALPHAREPLICATE
        base = swz(base, WWWW);
    if (ta & 0x10) { // COMPLEMENT
        a.op(O_ADD, {dst(T_TEMP, scratch), src(T_CONST, PC_LITERALS, XXXX), neg(base)});
        return src(T_TEMP, scratch);
    }
    return base;
}

// One D3DTEXTUREOP into r6 under `mask`.
void stage_op(Asm &a, uint32_t op, uint32_t mask, uint32_t a1, uint32_t a2, uint32_t a0) {
    const uint32_t d = dst(T_TEMP, R_RESULT, mask);
    const uint32_t one = src(T_CONST, PC_LITERALS, XXXX), half = src(T_CONST, PC_LITERALS, YYYY),
                   two = src(T_CONST, PC_LITERALS, ZZZZ), four = src(T_CONST, PC_LITERALS, WWWW);
    const uint32_t r7 = src(T_TEMP, 7), r8 = src(T_TEMP, 8);
    const uint32_t d7 = dst(T_TEMP, 7), d8 = dst(T_TEMP, 8);
    switch (op) {
    case 2:  // SELECTARG1
    case 17: // PREMODULATE: the next stage's texture is not known here
        a.op(O_MOV, {d, a1});
        break;
    case 3: // SELECTARG2
        a.op(O_MOV, {d, a2});
        break;
    case 4: // MODULATE
        a.op(O_MUL, {d, a1, a2});
        break;
    case 5: // MODULATE2X
    case 6: // MODULATE4X
        a.op(O_MUL, {d7, a1, a2});
        a.op(O_MUL, {d, r7, op == 5 ? two : four});
        break;
    case 7: // ADD
        a.op(O_ADD, {d, a1, a2});
        break;
    case 8: // ADDSIGNED
    case 9: // ADDSIGNED2X
        a.op(O_ADD, {d7, a1, a2});
        if (op == 8) {
            a.op(O_ADD, {d, r7, neg(half)});
        } else {
            a.op(O_ADD, {d7, r7, neg(half)});
            a.op(O_MUL, {d, r7, two});
        }
        break;
    case 10: // SUBTRACT
        a.op(O_ADD, {d, a1, neg(a2)});
        break;
    case 11: // ADDSMOOTH: a1 + a2 (1 - a1)
        a.op(O_ADD, {d7, one, neg(a1)});
        a.op(O_MAD, {d, a2, r7, a1});
        break;
    case 12: // BLENDDIFFUSEALPHA
        a.op(O_LRP, {d, src(T_INPUT, 0, WWWW), a1, a2});
        break;
    case 13: // BLENDTEXTUREALPHA
        a.op(O_LRP, {d, src(T_TEMP, R_TEXEL, WWWW), a1, a2});
        break;
    case 14: // BLENDFACTORALPHA
        a.op(O_LRP, {d, src(T_CONST, PC_TFACTOR, WWWW), a1, a2});
        break;
    case 15: // BLENDTEXTUREALPHAPM: a1 + a2 (1 - texture alpha)
        a.op(O_ADD, {d7, one, neg(src(T_TEMP, R_TEXEL, WWWW))});
        a.op(O_MAD, {d, a2, r7, a1});
        break;
    case 16: // BLENDCURRENTALPHA
        a.op(O_LRP, {d, src(T_TEMP, R_CURRENT, WWWW), a1, a2});
        break;
    case 18: // MODULATEALPHA_ADDCOLOR: a1 + a1.a a2
        a.op(O_MAD, {d, swz(a1, WWWW), a2, a1});
        break;
    case 19: // MODULATECOLOR_ADDALPHA: a1 a2 + a1.a
        a.op(O_MAD, {d, a1, a2, swz(a1, WWWW)});
        break;
    case 20: // MODULATEINVALPHA_ADDCOLOR: (1 - a1.a) a2 + a1
        a.op(O_ADD, {d7, one, neg(swz(a1, WWWW))});
        a.op(O_MAD, {d, r7, a2, a1});
        break;
    case 21: // MODULATEINVCOLOR_ADDALPHA: (1 - a1) a2 + a1.a
        a.op(O_ADD, {d7, one, neg(a1)});
        a.op(O_MAD, {d, r7, a2, swz(a1, WWWW)});
        break;
    case 24: // DOTPRODUCT3, into every channel
        a.op(O_ADD, {d7, a1, neg(half)});
        a.op(O_ADD, {d8, a2, neg(half)});
        a.op(O_DP3, {d7, r7, r8});
        a.op(O_MUL, {dst(T_TEMP, R_RESULT, M_ALL, true), r7, four});
        break;
    case 25: // MULTIPLYADD: a0 + a1 a2
        a.op(O_MAD, {d, a1, a2, a0});
        break;
    case 26: // LERP: a0 a1 + (1 - a0) a2
        a.op(O_LRP, {d, a0, a1, a2});
        break;
    default: // BUMPENVMAP and BUMPENVMAPLUMINANCE are not modelled: pass current
        a.op(O_MOV, {d, src(T_TEMP, R_CURRENT)});
        break;
    }
}

std::vector<uint8_t> build_ps(const PsState &s) {
    Asm a(0xffff0200u);
    a.def(PC_LITERALS, 1.0f, 0.5f, 2.0f, 4.0f);
    a.def(PC_ZERO, 0.0f, 0.0f, 0.0f, 0.0f);
    a.dcl(0, dst(T_INPUT, 0));
    a.dcl(0, dst(T_INPUT, 1));
    for (uint32_t i = 0; i < s.nstages; ++i)
        if (s.stage[i].sample) {
            a.dcl(0, dst(T_TEXTURE, i));
            a.dcl((s.stage[i].sample == 2 ? 3u : 2u) << 27, dst(T_SAMPLER, i));
        }
    a.op(O_MOV, {dst(T_TEMP, R_CURRENT), src(T_INPUT, 0)});
    a.op(O_MOV, {dst(T_TEMP, R_TEMPREG), src(T_CONST, PC_ZERO)});
    for (uint32_t i = 0; i < s.nstages; ++i) {
        const PsStage &st = s.stage[i];
        if (st.sample)
            a.op(O_TEX, {dst(T_TEMP, R_TEXEL), src(T_TEXTURE, i), src(T_SAMPLER, i)},
                 st.projected ? 1 : 0);
        bool sampled = st.sample != 0;
        uint32_t c1 = stage_arg(a, st.carg1, i, sampled, 3);
        uint32_t c2 = stage_arg(a, st.carg2, i, sampled, 4);
        uint32_t c0 = stage_arg(a, st.carg0, i, sampled, 5);
        stage_op(a, st.colorop, M_XYZ, c1, c2, c0);
        if (st.colorop != 24) {    // DOTPRODUCT3 already wrote the alpha
            if (st.alphaop <= 1) { // DISABLE: the alpha passes through
                a.op(O_MOV, {dst(T_TEMP, R_RESULT, M_W), src(T_TEMP, R_CURRENT)});
            } else {
                uint32_t x1 = stage_arg(a, st.aarg1, i, sampled, 3);
                uint32_t x2 = stage_arg(a, st.aarg2, i, sampled, 4);
                uint32_t x0 = stage_arg(a, st.aarg0, i, sampled, 5);
                stage_op(a, st.alphaop, M_W, x1, x2, x0);
            }
        }
        // Each stage's result is clamped, as the fixed-function hardware does.
        uint32_t into = st.result == 5 ? R_TEMPREG : R_CURRENT;
        a.op(O_MOV, {dst(T_TEMP, into, M_ALL, true), src(T_TEMP, R_RESULT)});
    }
    if (s.specular)
        a.op(O_ADD, {dst(T_TEMP, R_CURRENT, M_XYZ, true), src(T_TEMP, R_CURRENT), src(T_INPUT, 1)});
    a.op(O_MOV, {dst(T_COLOROUT, 0), src(T_TEMP, R_CURRENT)});
    return a.bytes();
}
} // namespace

// ---------------------------------------------------------------------------
// From the device state to programs and constants
// ---------------------------------------------------------------------------
namespace {
struct Cached {
    D9ShaderBytes bytes;
    uint64_t key = 0;
};
// Programs by the state that shaped them. A game uses a few dozen
// combinations, so the cache is never trimmed.
const Cached &cached_program(const void *state, size_t size, bool pixel) {
    static std::mutex lock;
    static auto *cache = new std::map<std::string, Cached>();
    std::string k(1, pixel ? 'p' : 'v');
    k.append((const char *)state, size);
    std::lock_guard<std::mutex> hold(lock);
    auto it = cache->find(k);
    if (it != cache->end())
        return it->second;
    std::vector<uint8_t> code =
        pixel ? build_ps(*(const PsState *)state) : build_vs(*(const VsState *)state);
    Cached c;
    c.key = d9sh::code_key(code.data(), code.size());
    c.bytes.bytes = std::make_shared<const std::vector<uint8_t>>(std::move(code));
    return (*cache)[k] = c;
}

uint8_t material_source(const D9Pipeline &pl, uint32_t rs, const Inputs &in) {
    if (!pl.rs[141]) // COLORVERTEX
        return 0;
    uint32_t v = pl.rs[rs];
    if (v == 1 && in.color[0])
        return 1;
    if (v == 2 && in.color[1])
        return 2;
    return 0;
}

void vertex_state(const D9Pipeline &pl, const Inputs &in, VsState &s,
                  std::vector<const D9Pipeline::Light *> &lights) {
    s.positiont = in.positiont;
    s.normal = in.normal && !in.positiont;
    s.color0 = in.color[0];
    s.color1 = in.color[1];
    memcpy(s.tex_dims, in.tex_dims, 8);
    s.lighting = !in.positiont && pl.rs[137] != 0;
    s.specular = s.lighting && pl.rs[29] != 0;
    s.local_viewer = pl.rs[142] != 0;
    if (s.lighting) {
        s.src_diffuse = material_source(pl, 145, in);
        s.src_specular = material_source(pl, 146, in);
        s.src_ambient = material_source(pl, 147, in);
        s.src_emissive = material_source(pl, 148, in);
        for (const auto &l : pl.lights) {
            if (!l.second.enabled || lights.size() >= 8)
                continue;
            uint32_t type = l.second.raw[0];
            if (type < 1 || type > 3)
                continue;
            s.light_type[lights.size()] = (uint8_t)type;
            lights.push_back(&l.second);
        }
        s.nlights = (uint8_t)lights.size();
    }
    // Vertex fog. A pre-transformed vertex brings its own fog factor in the
    // specular alpha.
    if (pl.rs[28] && pl.rs[35] == 0) { // FOGENABLE, FOGTABLEMODE none
        if (in.positiont)
            s.fog = 4;
        else if (pl.rs[140] >= 1 && pl.rs[140] <= 3)
            s.fog = (uint8_t)pl.rs[140];
    }
    s.range_fog = pl.rs[48] != 0;
    for (int i = 0; i < 8; ++i) {
        uint32_t tci = pl.tss[i][11], flags = pl.tss[i][24];
        s.tci_gen[i] = (uint8_t)std::min<uint32_t>(tci >> 16, 15);
        s.tci_index[i] = (uint8_t)(tci & 7);
        s.tt_count[i] = (uint8_t)(flags & 0xff) <= 4 ? (uint8_t)(flags & 0xff) : 0;
        s.tt_projected[i] = (flags & 0x100) ? 1 : 0;
    }
}

void vertex_constants(const D9Pipeline &pl, const VsState &s,
                      const std::vector<const D9Pipeline::Light *> &lights, const int32_t vp[4],
                      float (*c)[4]) {
    Mat world = mat(pl.transform[24]), view = mat(pl.transform[2]), proj = mat(pl.transform[3]);
    Mat wv = mul(world, view);
    put_columns(c, VC_WVP, mul(wv, proj));
    put_columns(c, VC_WV, wv);
    put_normal_matrix(c, VC_NORMAL, wv);
    for (int k = 0; k < 4; ++k)
        memcpy(c[VC_MAT + k], &pl.material[4 * k], 16);
    c[VC_MAT + 4][0] = pl.material[16];
    put_color(c[VC_AMBIENT], pl.rs[139]);
    float start = as_float(pl.rs[36]), end = as_float(pl.rs[37]);
    c[VC_FOG][0] = start;
    c[VC_FOG][1] = end;
    c[VC_FOG][2] = as_float(pl.rs[38]);
    c[VC_FOG][3] = std::fabs(end - start) > 1e-12f ? 1.0f / (end - start) : 0.0f;
    float w = vp[2] > 0 ? (float)vp[2] : 1.0f, h = vp[3] > 0 ? (float)vp[3] : 1.0f;
    c[VC_RHW][0] = 2.0f / w;
    c[VC_RHW][1] = -2.0f / h;
    c[VC_RHW][2] = -1.0f - 2.0f * (float)vp[0] / w;
    c[VC_RHW][3] = 1.0f + 2.0f * (float)vp[1] / h;
    for (size_t i = 0; i < lights.size(); ++i) {
        const uint32_t *r = lights[i]->raw;
        float (*l)[4] = c + VC_LIGHT + LIGHT_REGS * i;
        for (int k = 0; k < 4; ++k) {
            l[0][k] = as_float(r[1 + k]);
            l[1][k] = as_float(r[5 + k]);
            l[2][k] = as_float(r[9 + k]);
        }
        float p[3] = {as_float(r[13]), as_float(r[14]), as_float(r[15])};
        float d[3] = {as_float(r[16]), as_float(r[17]), as_float(r[18])};
        for (int k = 0; k < 3; ++k) {
            l[3][k] =
                p[0] * view.m[k] + p[1] * view.m[4 + k] + p[2] * view.m[8 + k] + view.m[12 + k];
            l[4][k] = -(d[0] * view.m[k] + d[1] * view.m[4 + k] + d[2] * view.m[8 + k]);
        }
        float len = std::sqrt(l[4][0] * l[4][0] + l[4][1] * l[4][1] + l[4][2] * l[4][2]);
        for (int k = 0; k < 3 && len > 0; ++k)
            l[4][k] /= len;
        float range = as_float(r[19]);
        l[3][3] = range > 0 ? range : 3.0e38f;
        float a0 = as_float(r[21]), a1 = as_float(r[22]), a2 = as_float(r[23]);
        if (a0 == 0 && a1 == 0 && a2 == 0)
            a0 = 1;
        l[5][0] = a0;
        l[5][1] = a1;
        l[5][2] = a2;
        float cos_theta = std::cos(as_float(r[24]) * 0.5f),
              cos_phi = std::cos(as_float(r[25]) * 0.5f);
        l[6][0] = cos_phi;
        l[6][1] = 1.0f / std::max(cos_theta - cos_phi, 1e-4f);
        l[6][2] = std::max(as_float(r[20]), 1e-3f);
    }
    for (int i = 0; i < 8; ++i)
        if (s.tt_count[i])
            put_columns(c, VC_TEXMAT + 4 * i, mat(pl.transform[16 + i]));
}

void pixel_state(const D9Pipeline &pl, uint32_t cube_mask, uint32_t bound_mask, PsState &s) {
    s.specular = pl.rs[29] != 0;
    for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t *t = pl.tss[i];
        if (t[1] <= 1) // COLOROP DISABLE ends the cascade
            break;
        PsStage &st = s.stage[i];
        st.colorop = (uint8_t)std::min<uint32_t>(t[1], 255);
        st.carg1 = (uint8_t)t[2];
        st.carg2 = (uint8_t)t[3];
        st.alphaop = (uint8_t)std::min<uint32_t>(t[4], 255);
        st.aarg1 = (uint8_t)t[5];
        st.aarg2 = (uint8_t)t[6];
        st.carg0 = (uint8_t)t[26];
        st.aarg0 = (uint8_t)t[27];
        st.result = (uint8_t)t[28];
        st.sample = (bound_mask >> i & 1) ? ((cube_mask >> i & 1) ? 2 : 1) : 0;
        st.projected = (t[24] & 0x100) ? 1 : 0;
        s.nstages = (uint8_t)(i + 1);
    }
}

void pixel_constants(const D9Pipeline &pl, float (*c)[4]) {
    put_color(c[PC_TFACTOR], pl.rs[60]);
    for (int i = 0; i < 8; ++i)
        put_color(c[PC_STAGE + i], pl.tss[i][32]);
}
} // namespace

bool d9_ffp_apply(const D9Pipeline &pl, const std::vector<uint8_t> &decl, uint32_t cube_mask,
                  uint32_t bound_mask, const int32_t viewport[4], D9Pipeline &out) {
    Inputs in = inputs_of(decl);
    bool need_vs = pl.vs.empty(), need_ps = pl.ps.empty();
    if (need_vs && !in.position && !in.positiont)
        return false;
    out = pl;
    if (need_vs) {
        VsState s;
        memset(&s, 0, sizeof s);
        std::vector<const D9Pipeline::Light *> lights;
        vertex_state(pl, in, s, lights);
        const Cached &prog = cached_program(&s, sizeof s, false);
        out.vs = prog.bytes;
        out.vs_key = prog.key;
        vertex_constants(pl, s, lights, viewport, out.vconst);
    }
    if (need_ps) {
        PsState s;
        memset(&s, 0, sizeof s);
        pixel_state(pl, cube_mask, bound_mask, s);
        const Cached &prog = cached_program(&s, sizeof s, true);
        out.ps = prog.bytes;
        out.ps_key = prog.key;
        pixel_constants(pl, out.pconst);
    }
    return true;
}
