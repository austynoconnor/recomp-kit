// Text for guest canvases: a TrueType face the program registered when its font
// names one, the bundled stand-in when it names a Windows interface face, else
// fixed 8x16 bitmap cells. No host font services or guest
// pointers escape this file; callbacks receive temporary guest-heap records.
#include "gdi32_internal.h"
#include <cstdlib>
#include "../platform/os.h"
#include "gdi32_font8x16.h"
#include "gdi32_truetype.h"
#include "memory.h"
#include "win32.h"
#include <algorithm>
#include <cstring>
#include <climits>
#include <map>
using namespace gdi;
namespace {
uint32_t font_word(const Object &font, int offset) {
    uint32_t v;
    memcpy(&v, font.logfont.data() + offset, 4);
    return v;
}
int32_t font_scale(uint32_t dc) {
    auto *d = dc_of(dc);
    auto it = objects().find(d ? d->font : 0);
    int64_t height = it == objects().end() ? 16 : int32_t(font_word(it->second, 0));
    return int32_t(std::max<int64_t>(1, std::abs(height) / 16));
}
void create_font(X86 *c) {
    uint32_t p = arg(c, 0);
    if (!p || !gm_valid(p, 92)) {
        set_eax(c, 0);
        return;
    }
    Object font;
    font.kind = Object::Font;
    memcpy(font.logfont.data(), g_mem + p, 92);
    set_eax(c, make_object(font));
}
// TEXTMETRICW is 60 bytes on x86: eleven DWORDs, four UTF-16
// characters, five BYTE fields and three bytes of trailing padding.
void metrics(uint32_t out, int32_t scale, uint32_t weight) {
    memset(g_mem + out, 0, 60);
    wr32(out, 16u * scale);
    wr32(out + 4, 13u * scale);
    wr32(out + 8, 3u * scale);
    wr32(out + 20, 8u * scale);
    wr32(out + 24, 8u * scale);
    wr32(out + 28, weight);
    wr32(out + 36, 96);
    wr32(out + 40, 96);
    wr16(out + 44, 32);
    wr16(out + 46, 127);
    wr16(out + 48, '?');
    wr16(out + 50, ' ');
    wr8(out + 55, 0x30);
    wr8(out + 56, 0);
}
// All wide text entry points share these bitmap glyphs and DC colour/clip rules.
void glyph(uint32_t hdc, int64_t x, int64_t y, uint32_t ch, bool underline = false) {
    auto *dc = dc_of(hdc);
    if (ch < 32 || ch > 127)
        ch = '?';
    int64_t scale = font_scale(hdc), width = 8 * scale, height = 16 * scale;
    Rect clip = clip_box(hdc);
    uint32_t fg = argb(dc->text_color), bg = argb(dc->bk_color);
    for (int64_t yy = std::max<int64_t>(y, clip.t); yy < std::min<int64_t>(y + height, clip.b);
         ++yy)
        for (int64_t xx = std::max<int64_t>(x, clip.l); xx < std::min<int64_t>(x + width, clip.r);
             ++xx) {
            uint8_t row = recomp_font::font8x16[ch - 32][size_t((yy - y) / scale)];
            if ((row & (0x80 >> ((xx - x) / scale))) || (underline && yy - y >= 14 * scale))
                write_pixel(hdc, xx, yy, fg);
            else if (dc->bk_mode == 2)
                write_pixel(hdc, xx, yy, bg);
        }
}
// How one DC's text is measured and drawn: from the registered TrueType face
// its font names, or from the bitmap cells.
struct Pen {
    const TrueTypeFace *face = nullptr;
    double scale = 0;
    int64_t cells = 1;
    TrueTypeMetrics metrics;
    int64_t advance(uint32_t ch) const {
        return face ? truetype_advance(face, scale, ch) : 8 * cells;
    }
    int64_t height() const {
        return face ? metrics.height : 16 * cells;
    }
    void draw(uint32_t hdc, int64_t x, int64_t y, uint32_t ch, bool underline) const {
        if (!face) {
            glyph(hdc, x, y, ch, underline);
            return;
        }
        auto *dc = dc_of(hdc);
        if (!dc)
            return;
        const uint32_t fg = argb(dc->text_color) & 0xffffffu;
        const int64_t width = advance(ch);
        if (dc->bk_mode == 2)
            fill(hdc, {int32_t(x), int32_t(y), int32_t(x + width), int32_t(y + metrics.height)},
                 argb(dc->bk_color));
        const TrueTypeGlyph &g = truetype_glyph(face, scale, ch);
        const int64_t baseline = y + metrics.ascent;
        for (int32_t j = 0; j < g.h; ++j)
            for (int32_t i = 0; i < g.w; ++i)
                if (uint8_t cover = g.coverage[size_t(j) * size_t(g.w) + size_t(i)])
                    write_pixel(hdc, x + g.x + i, baseline + g.y + j, (uint32_t(cover) << 24) | fg,
                                true);
        if (underline)
            for (int64_t yy = baseline + 1;
                 yy <= baseline + std::max<int64_t>(1, metrics.height / 16); ++yy)
                for (int64_t xx = x; xx < x + width; ++xx)
                    write_pixel(hdc, xx, yy, 0xff000000u | fg);
    }
};
Pen pen_of(uint32_t hdc) {
    Pen pen;
    pen.cells = font_scale(hdc);
    auto *dc = dc_of(hdc);
    auto it = objects().find(dc ? dc->font : 0);
    if (it == objects().end() || it->second.kind != Object::Font)
        return pen;
    std::string family;
    for (size_t i = 0; i < 32; ++i) {
        uint16_t unit =
            uint16_t(it->second.logfont[28 + 2 * i] | (it->second.logfont[29 + 2 * i] << 8));
        if (!unit)
            break;
        family.push_back(unit < 128 ? char(unit) : '?');
    }
    if (family.empty())
        return pen;
    pen.face = truetype_find(family);
    if (!pen.face) {
        // A face every Windows has and the program never supplied: what it
        // gets with no font of its own set, from the VCL's default font.
        const int32_t weight = int32_t(font_word(it->second, 16));
        pen.face = truetype_windows_substitute(family, weight);
        if (pen.face)
            log_once(("gdi-font:" + family).c_str(),
                     "gdi: \"%s\" is a Windows font the program did not register; drawing it "
                     "with the bundled Open Sans",
                     family.c_str());
    }
    if (!pen.face)
        return pen;
    pen.scale = truetype_scale(pen.face, int32_t(font_word(it->second, 0)));
    pen.metrics = truetype_metrics(pen.face, pen.scale);
    return pen;
}
void get_metrics(X86 *c) {
    auto *dc = dc_of(arg(c, 0));
    uint32_t out = arg(c, 1);
    if (!dc || !out || !gm_valid(out, 60)) {
        set_eax(c, 0);
        return;
    }
    auto font = objects().find(dc->font);
    uint32_t weight = font == objects().end() ? 400 : font_word(font->second, 16);
    if (!weight)
        weight = 400;
    metrics(out, font_scale(arg(c, 0)), weight);
    const Pen pen = pen_of(arg(c, 0));
    if (pen.face) {
        wr32(out, uint32_t(pen.metrics.height));
        wr32(out + 4, uint32_t(pen.metrics.ascent));
        wr32(out + 8, uint32_t(pen.metrics.descent));
        wr32(out + 12, uint32_t(pen.metrics.internal));
        wr32(out + 20, uint32_t(pen.metrics.average));
        wr32(out + 24, uint32_t(pen.metrics.maximum));
    }
    set_eax(c, 1);
}
void extent(X86 *c) {
    uint32_t dc = arg(c, 0), text = arg(c, 1), n = arg(c, 2), out = arg(c, 3);
    if (!dc_of(dc) || !out || !gm_valid(out, 8) || n > GUEST_SIZE / 2 ||
        (n && (!text || !gm_valid(text, n * 2)))) {
        set_eax(c, 0);
        return;
    }
    const Pen pen = pen_of(dc);
    uint64_t width = 0;
    for (uint32_t i = 0; i < n; ++i)
        width += uint64_t(pen.advance(rd16(text + 2 * i)));
    if (width > INT_MAX) {
        set_eax(c, 0);
        return;
    }
    wr32(out, uint32_t(width));
    wr32(out + 4, uint32_t(pen.height()));
    set_eax(c, 1);
}
void text_out(X86 *c) {
    uint32_t hdc = arg(c, 0), flags = arg(c, 3), rp = arg(c, 4), text = arg(c, 5), n = arg(c, 6),
             dx = arg(c, 7);
    // ETO_OPAQUE and ETO_CLIPPED with no rectangle are ignored, as Windows
    // does (the DirectX SDK's CD3DFont passes ETO_OPAQUE and NULL).
    if (!rp)
        flags &= ~6u;
    auto *dc = dc_of(hdc);
    int w, h;
    if (!dc || !dc_size(hdc, &w, &h) || n > GUEST_SIZE / 4 ||
        (n && (!text || !gm_valid(text, n * 2))) || (dx && !gm_valid(dx, n * 4)) ||
        ((flags & 6) && (!rp || !gm_valid(rp, 16)))) {
        set_eax(c, 0);
        return;
    }
    bool old_clipped = dc->clipped;
    std::vector<Rect> old_clip = dc->clip;
    Rect rect{};
    if (flags & 6)
        rect = {int32_t(rd32(rp)), int32_t(rd32(rp + 4)), int32_t(rd32(rp + 8)),
                int32_t(rd32(rp + 12))};
    if (flags & 4) {
        Rect cut = to_device(hdc, rect);
        std::vector<Rect> source = dc->clipped ? dc->clip : std::vector<Rect>{{0, 0, w, h}};
        dc->clip.clear();
        dc->clipped = true;
        for (Rect r : source) {
            Rect hit{std::max(r.l, cut.l), std::max(r.t, cut.t), std::min(r.r, cut.r),
                     std::min(r.b, cut.b)};
            if (hit.l < hit.r && hit.t < hit.b)
                dc->clip.push_back(hit);
        }
    }
    uint32_t bg = argb(dc->bk_color);
    if (flags & 2)
        fill(hdc, rect, bg);
    const Pen pen = pen_of(hdc);
    int64_t x = int32_t(arg(c, 1)), y = int32_t(arg(c, 2));
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t unit = rd16(text + 2 * i);
        pen.draw(hdc, x, y, unit, false);
        x += dx ? int32_t(rd32(dx + 4 * i)) : pen.advance(unit);
    }
    dc->clip = std::move(old_clip);
    dc->clipped = old_clipped;
    set_eax(c, 1);
}
// The fonts are kept, so a DC whose font names one of them draws with it. Data
// the rasterizer cannot read still gets a handle, as it always has here.
void memory_font(X86 *c) {
    uint32_t data = arg(c, 0), size = arg(c, 1), count = arg(c, 3);
    if (count && !gm_valid(count, 4)) {
        set_eax(c, 0);
        return;
    }
    uint32_t fonts =
        data && size && gm_valid(data, size) ? truetype_add_memory(g_mem + data, size) : 0;
    Object font;
    font.kind = Object::Font;
    uint32_t handle = make_object(font);
    if (count)
        wr32(count, std::max<uint32_t>(fonts, 1));
    set_eax(c, handle);
}
void enumerate(X86 *c) {
    uint32_t cb = arg(c, 2), param = arg(c, 3);
    if (!dc_of(arg(c, 0)) || !cb) {
        set_eax(c, 0);
        return;
    }
    uint32_t data = heap_alloc(92 + 60, true);
    if (!data) {
        set_eax(c, 0);
        return;
    }
    wr32(data, 16);
    wr32(data + 4, 8);
    wr32(data + 16, 400);
    wr8(data + 27, 0x30);
    gm_put_wstr(data + 28, "recomp", 32);
    metrics(data + 92, 1, 400);
    uint32_t result = guest_call(c, cb, data, data + 92, 1, param);
    heap_free(data);
    set_eax(c, result);
}
void unsupported(X86 *c) {
    set_eax(c, 0);
}

// The ANSI text entry points: their strings widened through the ANSI code page
// (Latin-1 here) into a temporary guest block, then the wide entry point.
uint32_t widen_ansi(uint32_t text, uint32_t n) {
    uint32_t w = heap_alloc(2 * n + 2, true);
    for (uint32_t i = 0; w && i < n; ++i)
        wr16(w + 2 * i, rd8(text + i));
    return w;
}
// CreateFontA(cHeight, cWidth, cEscapement, cOrientation, cWeight, bItalic,
// bUnderline, bStrikeOut, iCharSet, iOutPrecision, iClipPrecision, iQuality,
// iPitchAndFamily, pszFaceName): the LOGFONTW the wide path keeps.
void create_font_a(X86 *c) {
    uint32_t lf = heap_alloc(92, true);
    if (!lf) {
        set_eax(c, 0);
        return;
    }
    for (int i = 0; i < 5; ++i)
        wr32(lf + 4u * (uint32_t)i, arg(c, i));
    for (int i = 0; i < 8; ++i)
        wr8(lf + 20u + (uint32_t)i, (uint8_t)arg(c, 5 + i));
    if (uint32_t face = arg(c, 13))
        for (uint32_t i = 0; i < 31 && rd8(face + i); ++i)
            wr16(lf + 28 + 2 * i, rd8(face + i));
    shim_forward(c, create_font, {lf});
    heap_free(lf);
}
// CreateFontIndirectA(const LOGFONTA *): 28 bytes of metrics, then a 32-byte face.
void create_font_indirect_a(X86 *c) {
    uint32_t src = arg(c, 0), lf = src && gm_valid(src, 60) ? heap_alloc(92, true) : 0;
    if (!lf) {
        set_eax(c, 0);
        return;
    }
    for (uint32_t i = 0; i < 28; i += 4)
        wr32(lf + i, rd32(src + i));
    for (uint32_t i = 0; i < 31 && rd8(src + 28 + i); ++i)
        wr16(lf + 28 + 2 * i, rd8(src + 28 + i));
    shim_forward(c, create_font, {lf});
    heap_free(lf);
}
// ExtTextOutA(hdc, x, y, options, lprect, lpString, c, lpDx)
void text_out_a(X86 *c) {
    uint32_t n = arg(c, 6), text = arg(c, 5);
    if (n > GUEST_SIZE / 4 || (n && (!text || !gm_valid(text, n)))) {
        set_eax(c, 0);
        return;
    }
    uint32_t w = widen_ansi(text, n);
    shim_forward(c, text_out,
                 {arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), w, n, arg(c, 7)});
    heap_free(w);
}
// GetTextExtentPoint32A(hdc, lpString, c, lpSize)
void extent_a(X86 *c) {
    uint32_t n = arg(c, 2), text = arg(c, 1);
    if (n > GUEST_SIZE / 4 || (n && (!text || !gm_valid(text, n)))) {
        set_eax(c, 0);
        return;
    }
    uint32_t w = widen_ansi(text, n);
    shim_forward(c, extent, {arg(c, 0), w, n, arg(c, 3)});
    heap_free(w);
}
// SetTextAlign and SetMapMode keep what was asked per DC and return the
// previous value. The text path draws from the reference point's top-left and
// in device units, which is TA_TOP | TA_LEFT and MM_TEXT, the defaults; any
// other request is reported once.
std::map<uint32_t, uint32_t> &text_align() {
    static auto *m = new std::map<uint32_t, uint32_t>();
    return *m;
}
std::map<uint32_t, uint32_t> &map_mode() {
    static auto *m = new std::map<uint32_t, uint32_t>();
    return *m;
}
void set_text_align(X86 *c) {
    uint32_t dc = arg(c, 0), align = arg(c, 1);
    if (!dc_of(dc)) {
        set_eax(c, 0xffffffffu); // GDI_ERROR
        return;
    }
    auto it = text_align().find(dc);
    uint32_t old = it == text_align().end() ? 0 : it->second;
    text_align()[dc] = align;
    if (align & ~1u) // anything beyond TA_UPDATECP's absence and TA_TOP|TA_LEFT
        log_once("gdi.textalign", "gdi: SetTextAlign(%x) is recorded; text still draws top-left",
                 align);
    set_eax(c, old);
}
void get_text_align(X86 *c) {
    auto it = text_align().find(arg(c, 0));
    set_eax(c, it == text_align().end() ? 0 : it->second);
}
void set_map_mode(X86 *c) {
    uint32_t dc = arg(c, 0), mode = arg(c, 1);
    if (!dc_of(dc) || mode < 1 || mode > 8) {
        set_eax(c, 0);
        return;
    }
    auto it = map_mode().find(dc);
    uint32_t old = it == map_mode().end() ? 1 : it->second;
    map_mode()[dc] = mode;
    if (mode != 1)
        log_once("gdi.mapmode", "gdi: SetMapMode(%u) is recorded; drawing stays in MM_TEXT", mode);
    set_eax(c, old);
}
void get_map_mode(X86 *c) {
    auto it = map_mode().find(arg(c, 0));
    set_eax(c, dc_of(arg(c, 0)) ? (it == map_mode().end() ? 1 : it->second) : 0);
}
} // namespace
namespace gdi {
// DrawText's basic VCL layout uses the same fixed font as ExtTextOutW.
// Measurement must use that font too; CALCRECT never mutates canvas pixels.
uint32_t draw_text(uint32_t hdc, uint32_t text, uint32_t count, uint32_t rp, uint32_t flags) {
    auto *dc = dc_of(hdc);
    if (recomp_env("TRACE_GDI")) {
        int dw = -1, dh = -1;
        dc_size(hdc, &dw, &dh);
        LOGW("gdi: DrawText hdc=%08x(%dx%d)%s flags=%08x \"%s\"", hdc, dw, dh,
             dc ? "" : " NO SUCH DC", flags, gm_wstr(text, 64).c_str());
    }
    if (!dc || !rp || !gm_valid(rp, 16) || !text)
        return 0;
    if (count == UINT32_MAX) {
        count = 0;
        while (uint64_t(text) + 2ull * count + 2 <= GUEST_SIZE && gm_valid(text + count * 2, 2) &&
               rd16(text + count * 2))
            ++count;
    }
    if (!count || count > GUEST_SIZE / 2 || !gm_valid(text, count * 2))
        return 0;
    Rect rect{int32_t(rd32(rp)), int32_t(rd32(rp + 4)), int32_t(rd32(rp + 8)),
              int32_t(rd32(rp + 12))};
    const Pen pen = pen_of(hdc);
    const int64_t ch = pen.height();
    struct Cell {
        uint16_t ch;
        bool underline;
    };
    std::vector<std::vector<Cell>> lines(1);
    auto width_of = [&](const std::vector<Cell> &line) {
        int64_t width = 0;
        for (Cell cell : line)
            width += pen.advance(cell.ch);
        return width;
    };
    bool prefix = false;
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t unit = rd16(text + 2 * i);
        if (!(flags & 0x800) && unit == '&') { // DT_NOPREFIX
            if (i + 1 < count && rd16(text + 2 * (i + 1)) == '&')
                ++i;
            else {
                prefix = true;
                continue;
            }
        }
        if (unit == '\r' || unit == '\n') {
            if (unit == '\r' && i + 1 < count && rd16(text + 2 * (i + 1)) == '\n')
                ++i;
            if (!(flags & 0x20)) {
                lines.emplace_back();
                prefix = false;
                continue;
            }
            unit = ' ';
        }
        if (unit == '\t' && (flags & 0x40)) {
            size_t spaces = 8 - lines.back().size() % 8;
            lines.back().insert(lines.back().end(), spaces, Cell{' ', false});
        } else
            lines.back().push_back({unit, prefix && !(flags & 0x100000)});
        prefix = false;
    }
    if ((flags & 0x10) && !(flags & 0x20) && rect.r > rect.l) { // DT_WORDBREAK
        const int64_t limit = int64_t(rect.r) - rect.l;
        for (size_t i = 0; i < lines.size(); ++i) {
            auto &line = lines[i];
            size_t cols = 0; // how many characters fit the width
            for (int64_t used = 0; cols < line.size(); ++cols) {
                used += pen.advance(line[cols].ch);
                if (used > limit)
                    break;
            }
            if (!cols || cols >= line.size())
                continue;
            size_t split = std::min(cols, line.size() - 1);
            while (split && line[split].ch != ' ')
                --split;
            if (!split) { // A long word is never split in the middle.
                split = cols;
                while (split < line.size() && line[split].ch != ' ')
                    ++split;
            }
            if (split < line.size()) {
                std::vector<Cell> tail(line.begin() + split + 1, line.end());
                line.resize(split);
                lines.insert(lines.begin() + i + 1, std::move(tail));
            }
        }
    }
    int64_t height = ch * lines.size(), maxwidth = 0;
    for (const auto &line : lines)
        maxwidth = std::max(maxwidth, width_of(line));
    if (height > INT_MAX || maxwidth > INT_MAX)
        return 0;
    if (flags & 0x400) {
        wr32(rp + 8, uint32_t(int64_t(rect.l) + maxwidth));
        wr32(rp + 12, uint32_t(int64_t(rect.t) + height));
        return uint32_t(height);
    }
    int w, h;
    if (!dc_size(hdc, &w, &h))
        return 0;
    bool old_clipped = dc->clipped;
    std::vector<Rect> old_clip = dc->clip;
    if (!(flags & 0x100)) { // DT_NOCLIP
        Rect cut = to_device(hdc, rect);
        std::vector<Rect> source = dc->clipped ? dc->clip : std::vector<Rect>{{0, 0, w, h}};
        dc->clip.clear();
        dc->clipped = true;
        for (Rect r : source) {
            Rect hit{std::max(r.l, cut.l), std::max(r.t, cut.t), std::min(r.r, cut.r),
                     std::min(r.b, cut.b)};
            if (hit.l < hit.r && hit.t < hit.b)
                dc->clip.push_back(hit);
        }
    }
    int64_t y = rect.t;
    if (flags & 0x20) {
        if (flags & 4)
            y += (int64_t(rect.b) - rect.t - height) / 2;
        else if (flags & 8)
            y = int64_t(rect.b) - height;
    }
    for (const auto &line : lines) {
        int64_t x = rect.l, width = width_of(line);
        if (flags & 1)
            x += (int64_t(rect.r) - rect.l - width) / 2;
        else if (flags & 2)
            x = int64_t(rect.r) - width;
        for (Cell cell : line) {
            pen.draw(hdc, x, y, cell.ch, cell.underline);
            x += pen.advance(cell.ch);
        }
        y += ch;
    }
    dc->clip = std::move(old_clip);
    dc->clipped = old_clipped;
    return uint32_t(y - rect.t);
}
void register_text() {
#define G(n, a, f)                                                                                 \
    {                                                                                              \
        "GDI32.dll", n, a, f                                                                       \
    }
    static const ImportShim shims[] = {G("CreateFontIndirectW", 1, create_font),
                                       G("CreateFontA", 14, create_font_a),
                                       G("CreateFontIndirectA", 1, create_font_indirect_a),
                                       G("ExtTextOutA", 8, text_out_a),
                                       G("GetTextExtentPoint32A", 4, extent_a),
                                       G("SetTextAlign", 2, set_text_align),
                                       G("GetTextAlign", 1, get_text_align),
                                       G("SetMapMode", 2, set_map_mode),
                                       G("GetMapMode", 1, get_map_mode),
                                       G("ExtTextOutW", 8, text_out),
                                       G("GetTextExtentPoint32W", 4, extent),
                                       G("GetTextExtentPointW", 4, extent),
                                       G("GetTextMetricsW", 2, get_metrics),
                                       G("AddFontMemResourceEx", 4, memory_font),
                                       G("EnumFontsW", 4, enumerate),
                                       G("EnumFontFamiliesExW", 5, enumerate),
                                       G("CreateEnhMetaFileW", 4, unsupported),
                                       G("CreateEnhMetaFileA", 4, unsupported),
                                       G("CloseEnhMetaFile", 1, unsupported),
                                       G("DeleteEnhMetaFile", 1, unsupported),
                                       G("GetEnhMetaFileW", 1, unsupported),
                                       G("GetEnhMetaFileA", 1, unsupported),
                                       G("GetEnhMetaFileBits", 3, unsupported),
                                       G("GetEnhMetaFileHeader", 3, unsupported),
                                       G("GetEnhMetaFileDescriptionW", 3, unsupported),
                                       G("GetEnhMetaFileDescriptionA", 3, unsupported),
                                       G("GetEnhMetaFilePaletteEntries", 3, unsupported),
                                       G("GetEnhMetaFilePixelFormat", 3, unsupported),
                                       G("SetEnhMetaFileBits", 2, unsupported),
                                       G("SetWinMetaFileBits", 4, unsupported),
                                       G("GetWinMetaFileBits", 5, unsupported),
                                       G("PlayEnhMetaFile", 3, unsupported),
                                       G("CopyEnhMetaFileW", 2, unsupported),
                                       G("CreateDCW", 4, unsupported),
                                       G("CreateICW", 4, unsupported),
                                       G("StartDocW", 2, unsupported),
                                       G("EndDoc", 1, unsupported),
                                       G("StartPage", 1, unsupported),
                                       G("EndPage", 1, unsupported),
                                       G("AbortDoc", 1, unsupported),
                                       G("SetAbortProc", 2, unsupported)};
#undef G
    imports_register(shims, sizeof(shims) / sizeof(shims[0]));
}
} // namespace gdi
