// compat_2008.cpp - small imports of a 2008-era Visual C++ 2005 game.
//
// Bully: Scholarship Edition is the first game here to need them; each is
// answered the way Windows answers a full-screen game with no console, one
// display, no common-controls dialogs on screen and one processor's worth of
// OpenMP:
//
//  - KERNEL32 console calls: the process has no console, so GetConsoleMode
//    fails with ERROR_INVALID_HANDLE and the CRT falls back to WriteFile.
//    WriteConsole fails the same way if it is reached anyway.
//  - USER32 display and window calls: ChangeDisplaySettingsA reports success
//    (the host owns the real mode), GetMonitorInfoA describes the one display
//    the host presents, MoveWindow moves nothing and succeeds, and there are
//    no accelerator tables to translate.
//  - USER32 dialog calls: no dialog template is ever shown. DialogBoxIndirect-
//    ParamA returns -1 (creation failed), which callers treat as "cancelled";
//    the item calls on a dialog that does not exist return 0.
//  - COMCTL32 InitCommonControlsEx succeeds: the classes it registers are
//    only used by dialogs, which are not shown.
//  - ole32 CoSetProxyBlanket succeeds; it is only ever applied to a proxy
//    the caller already holds, and no class here hands out remote proxies.
//  - vcomp (the Visual C++ OpenMP runtime): a parallel region runs its body
//    once on the calling thread, and a static loop gives that one thread the
//    whole iteration range. Results are identical to OpenMP with one thread.
#include "imports.h"
#include "user32_internal.h"
#include "win32.h"

#include <stdio.h>
#include <iterator>
#include <string>

namespace {

const uint32_t ERROR_INVALID_HANDLE_ = 6;

void console_unavailable(X86 *c) {
    set_last_error(ERROR_INVALID_HANDLE_);
    set_eax(c, 0);
}

void succeed(X86 *c) {
    set_eax(c, 1);
}

void zero(X86 *c) {
    set_eax(c, 0);
}

// ChangeDisplaySettingsA(lpDevMode, dwFlags): DISP_CHANGE_SUCCESSFUL.
void change_display_settings(X86 *c) {
    set_eax(c, 0);
}

// GetMonitorInfoA(hMonitor, LPMONITORINFO): MONITORINFO is 40 bytes,
// MONITORINFOEXA adds a 32-byte ANSI device name.
void monitor_info_a(X86 *c) {
    uint32_t p = arg(c, 1);
    if (!p || !gm_valid(p, 40) || rd32(p) < 40) {
        set_eax(c, 0);
        return;
    }
    user32::display_rect(p + 4);
    user32::display_rect(p + 20);
    wr32(p + 36, 1); // MONITORINFOF_PRIMARY
    if (rd32(p) >= 72 && gm_valid(p, 72)) {
        static const char name[] = "\\\\.\\DISPLAY1";
        for (uint32_t i = 0; i < 32; ++i)
            wr8(p + 40 + i, i < sizeof name ? (uint8_t)name[i] : 0);
    }
    set_eax(c, 1);
}

void dialog_not_shown(X86 *c) {
    set_eax(c, 0xffffffffu);
}

// _vcomp_fork(BOOL ifval, int nargs, void (*body)(...), ...): cdecl, the
// body's arguments follow on the stack.
void vcomp_fork(X86 *c) {
    uint32_t nargs = arg(c, 1), body = arg(c, 2);
    uint32_t args[32];
    if (nargs > std::size(args)) {
        LOGW("vcomp: _vcomp_fork with %u arguments; the region was not run", nargs);
        return;
    }
    for (uint32_t i = 0; i < nargs; ++i)
        args[i] = arg(c, 3 + (int)i);
    if (body)
        guest_call(c, body, args, (int)nargs);
}

// _vcomp_for_static_simple_init(first, last, step, increment, *begin, *end):
// one thread owns the whole range, bounds inclusive as the compiler emits.
void vcomp_for_static_simple_init(X86 *c) {
    uint32_t first = arg(c, 0), last = arg(c, 1), begin = arg(c, 4), end = arg(c, 5);
    if (begin && gm_valid(begin, 4))
        wr32(begin, first);
    if (end && gm_valid(end, 4))
        wr32(end, last);
}

void nothing(X86 *) {}

// wsprintfW(LPWSTR out, LPCWSTR format, ...): cdecl. The USER32 subset:
// %[-][0][width](c|C|d|i|u|x|X|s|S) with the l/h size prefixes; output is
// capped at 1024 characters as Windows caps it.
void wsprintf_w(X86 *c) {
    uint32_t out = arg(c, 0), fmt = arg(c, 1);
    int next = 2;
    std::u16string r;
    auto wch = [](uint32_t a) { return (char16_t)rd16(a); };
    for (uint32_t p = fmt; gm_valid(p, 2) && wch(p) && r.size() < 1024; p += 2) {
        char16_t ch = wch(p);
        if (ch != u'%') {
            r += ch;
            continue;
        }
        p += 2;
        bool left = false, zero_pad = false, wide = true;
        if (wch(p) == u'-') {
            left = true;
            p += 2;
        }
        if (wch(p) == u'0') {
            zero_pad = true;
            p += 2;
        }
        uint32_t width = 0;
        while (wch(p) >= u'0' && wch(p) <= u'9') {
            width = width * 10 + (uint32_t)(wch(p) - u'0');
            p += 2;
        }
        if (wch(p) == u'l') {
            p += 2;
        } else if (wch(p) == u'h') {
            wide = false;
            p += 2;
        }
        char16_t conv = wch(p);
        std::u16string field;
        uint32_t v = conv == u'%' ? 0 : arg(c, next++);
        char buf[16];
        switch (conv) {
        case u'%':
            field = u"%";
            break;
        case u'c':
            field = std::u16string(1, (char16_t)(v & 0xffff));
            break;
        case u'C':
            field = std::u16string(1, (char16_t)(v & 0xff));
            break;
        case u'd':
        case u'i':
            snprintf(buf, sizeof buf, "%d", (int32_t)v);
            break;
        case u'u':
            snprintf(buf, sizeof buf, "%u", v);
            break;
        case u'x':
            snprintf(buf, sizeof buf, "%x", v);
            break;
        case u'X':
            snprintf(buf, sizeof buf, "%X", v);
            break;
        case u's':
        case u'S': {
            bool w = (conv == u's') == wide;
            if (!v) {
                field = u"(null)";
            } else if (w) {
                for (uint32_t s = v; gm_valid(s, 2) && rd16(s) && field.size() < 1024; s += 2)
                    field += (char16_t)rd16(s);
            } else {
                for (uint32_t s = v; gm_valid(s, 1) && rd8(s) && field.size() < 1024; ++s)
                    field += (char16_t)rd8(s);
            }
            break;
        }
        default:
            field = std::u16string(1, conv);
            --next;
            break;
        }
        if (field.empty() && conv != u'%')
            for (const char *b = buf; *b; ++b)
                field += (char16_t)*b;
        while (field.size() < width)
            field = left ? field + u' ' : (zero_pad && !left ? u'0' + field : u' ' + field);
        r += field;
        if (!wch(p))
            break;
    }
    if (r.size() > 1024)
        r.resize(1024);
    if (!out || !gm_valid(out, 2u * (uint32_t)(r.size() + 1))) {
        set_eax(c, 0);
        return;
    }
    for (size_t i = 0; i < r.size(); ++i)
        wr16(out + 2u * (uint32_t)i, r[i]);
    wr16(out + 2u * (uint32_t)r.size(), 0);
    set_eax(c, (uint32_t)r.size());
}

const ImportShim shims[] = {
    {"KERNEL32.dll", "GetConsoleMode", 2, console_unavailable},
    {"KERNEL32.dll", "WriteConsoleA", 5, console_unavailable},
    {"KERNEL32.dll", "WriteConsoleW", 5, console_unavailable},
    {"USER32.dll", "ChangeDisplaySettingsA", 2, change_display_settings},
    {"USER32.dll", "GetMonitorInfoA", 2, monitor_info_a},
    {"USER32.dll", "MoveWindow", 6, succeed},
    {"USER32.dll", "TranslateAcceleratorA", 3, zero},
    {"USER32.dll", "mouse_event", 5, nothing},
    {"USER32.dll", "wsprintfW", ARGC_CDECL, wsprintf_w},
    {"USER32.dll", "DialogBoxIndirectParamA", 5, dialog_not_shown},
    {"USER32.dll", "EndDialog", 2, zero},
    {"USER32.dll", "CheckDlgButton", 3, zero},
    {"USER32.dll", "SendDlgItemMessageA", 5, zero},
    {"COMCTL32.dll", "InitCommonControlsEx", 1, succeed},
    {"ole32.dll", "CoSetProxyBlanket", 8, [](X86 *c) { set_eax(c, 0); }},
    {"vcomp.dll", "_vcomp_fork", ARGC_CDECL, vcomp_fork},
    {"vcomp.dll", "_vcomp_for_static_simple_init", ARGC_CDECL, vcomp_for_static_simple_init},
    {"vcomp.dll", "_vcomp_for_static_end", ARGC_CDECL, nothing},
};

} // namespace

void compat_2008_register() {
    imports_register(shims, std::size(shims));
}
