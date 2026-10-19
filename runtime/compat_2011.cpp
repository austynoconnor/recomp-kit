// compat_2011.cpp - small imports of a 2010-era Visual C++ 2008 game that the
// other shim tables do not cover. Fallout: New Vegas (Gamebryo) is the first
// game here to need them; each is answered the way Windows answers a
// full-screen game with one display, no console and nobody else's windows:
//
//  - KERNEL32: CompareFileTime compares; FormatMessageA formats "Error N" the
//    way FormatMessageW already does; InitializeCriticalSectionAndSpinCount
//    is InitializeCriticalSection (one guest thread runs at a time, so a spin
//    count changes nothing); SetThreadIdealProcessor accepts any processor;
//    InterlockedExchangeAdd adds.
//  - USER32: one display device (\\.\DISPLAY1, attached and primary); a
//    window hook is accepted and never called (the host delivers input
//    through the window procedure, not low-level hooks); window text and
//    class queries answer empty; SwapMouseButton reports buttons unswapped.
//  - SHELL32 CommandLineToArgvW splits the command line by Windows' rules
//    into one LocalAlloc block the caller frees with LocalFree.
//  - ole32 CoFreeUnusedLibraries: no in-process server is ever loaded.
//  - COMCTL32 image lists: none is created (they only serve dialogs).
//  - d3d9 D3DPERF_SetOptions: there is no profiler to tell.
#include "imports.h"
#include "memory.h"
#include "win32.h"

#include <stdio.h>
#include <string.h>

#include <iterator>
#include <string>
#include <vector>

namespace {

// CompareFileTime(a, b): -1, 0 or 1 by the unsigned 64-bit tick counts.
void compare_file_time(X86 *c) {
    uint32_t a = arg(c, 0), b = arg(c, 1);
    if (!a || !b || !gm_valid(a, 8) || !gm_valid(b, 8)) {
        set_eax(c, 0);
        return;
    }
    uint64_t x = (uint64_t)rd32(a) | ((uint64_t)rd32(a + 4) << 32);
    uint64_t y = (uint64_t)rd32(b) | ((uint64_t)rd32(b + 4) << 32);
    set_eax(c, x < y ? 0xffffffffu : x > y ? 1u : 0u);
}

// FormatMessageA(flags, source, id, lang, buffer, size, args): only
// FORMAT_MESSAGE_FROM_SYSTEM is answered, as "Error <id>".
void format_message_a(X86 *c) {
    uint32_t flags = arg(c, 0), out = arg(c, 4), cap = arg(c, 5);
    if (!(flags & 0x1000) || (flags & (0x400 | 0x800)) || !out) {
        set_last_error(87);
        set_eax(c, 0);
        return;
    }
    std::string text = "Error " + std::to_string(arg(c, 2));
    uint32_t need = (uint32_t)text.size() + 1;
    if (flags & 0x100) { // FORMAT_MESSAGE_ALLOCATE_BUFFER
        cap = cap > need ? cap : need;
        if (!gm_valid(out, 4)) {
            set_eax(c, 0);
            return;
        }
        uint32_t buffer = heap_alloc(cap, true);
        if (!buffer) {
            set_last_error(8);
            set_eax(c, 0);
            return;
        }
        wr32(out, buffer);
        out = buffer;
    } else if (cap < need || !gm_valid(out, need)) {
        set_last_error(122);
        set_eax(c, 0);
        return;
    }
    set_eax(c, gm_put_str(out, text.c_str(), cap));
}

// The same CRITICAL_SECTION layout kernel32.cpp's InitializeCriticalSection
// writes: 24 bytes, LockCount (-1, unowned) at +4.
void init_critsec_spin(X86 *c) {
    uint32_t p = arg(c, 0);
    if (p && gm_valid(p, 24)) {
        memset(g_mem + p, 0, 24);
        wr32(p + 4, 0xffffffffu);
    }
    set_eax(c, 1);
}

// SetThreadIdealProcessor returns the previous ideal processor.
void set_thread_ideal_processor(X86 *c) {
    set_eax(c, 0);
}

void interlocked_exchange_add(X86 *c) {
    uint32_t p = arg(c, 0), v = arg(c, 1);
    uint32_t old = rd32(p);
    wr32(p, old + v);
    set_eax(c, old);
}

// EnumDisplayDevicesA(device, index, DISPLAY_DEVICEA *, flags). The struct
// is cb, DeviceName[32], DeviceString[128], StateFlags, DeviceID[128],
// DeviceKey[128]: 424 bytes. Only adapter 0 exists, and asking about its
// monitors (device named) finds one as well.
void enum_display_devices_a(X86 *c) {
    uint32_t index = arg(c, 1), out = arg(c, 2);
    if (index != 0 || !out || !gm_valid(out, 4) || rd32(out) < 424 || !gm_valid(out, 424)) {
        set_eax(c, 0);
        return;
    }
    bool monitor = arg(c, 0) != 0;
    memset(g_mem + out + 4, 0, 420);
    gm_put_str(out + 4, monitor ? "\\\\.\\DISPLAY1\\Monitor0" : "\\\\.\\DISPLAY1", 32);
    gm_put_str(out + 36, monitor ? "Generic PnP Monitor" : "Recomp Display Adapter", 128);
    wr32(out + 164, monitor ? 3u : 5u); // ACTIVE|ATTACHED, or ATTACHED_TO_DESKTOP|PRIMARY
    gm_put_str(out + 168, monitor ? "MONITOR\\Default_Monitor" : "PCI\\VEN_0000&DEV_0000", 128);
    set_eax(c, 1);
}

void set_windows_hook_ex(X86 *c) {
    static uint32_t next = 0x484f4f00u; // 'HOO'
    set_eax(c, ++next);
}

// GetWindowTextA / GetClassNameA(hwnd, buffer, size): an empty string.
void empty_text(X86 *c) {
    uint32_t buf = arg(c, 1), size = arg(c, 2);
    if (buf && size && gm_valid(buf, 1))
        wr8(buf, 0);
    set_eax(c, 0);
}

void zero(X86 *c) {
    set_eax(c, 0);
}
void one(X86 *c) {
    set_eax(c, 1);
}
void nothing(X86 *) {}

// Windows' argv rules: blanks separate arguments outside quotes; 2n
// backslashes before a quote are n backslashes and the quote toggles; 2n+1
// are n and a literal quote; other backslashes are literal. The program name
// (argument 0) ends at the first blank or at its closing quote.
std::vector<std::string> split_command_line(const std::string &s) {
    std::vector<std::string> out;
    size_t i = 0, n = s.size();
    std::string cur;
    if (i < n && s[i] == '"') {
        ++i;
        while (i < n && s[i] != '"')
            cur += s[i++];
        if (i < n)
            ++i;
    } else {
        while (i < n && s[i] != ' ' && s[i] != '\t')
            cur += s[i++];
    }
    out.push_back(cur);
    for (;;) {
        while (i < n && (s[i] == ' ' || s[i] == '\t'))
            ++i;
        if (i >= n)
            break;
        cur.clear();
        bool quoted = false;
        while (i < n && (quoted || (s[i] != ' ' && s[i] != '\t'))) {
            size_t slashes = 0;
            while (i < n && s[i] == '\\') {
                ++slashes;
                ++i;
            }
            if (i < n && s[i] == '"') {
                cur.append(slashes / 2, '\\');
                if (slashes % 2) {
                    cur += '"';
                } else if (quoted && i + 1 < n && s[i + 1] == '"') {
                    cur += '"';
                    ++i;
                } else {
                    quoted = !quoted;
                }
                ++i;
            } else {
                cur.append(slashes, '\\');
                if (i < n && (quoted || (s[i] != ' ' && s[i] != '\t')))
                    cur += s[i++];
            }
        }
        out.push_back(cur);
    }
    return out;
}

// CommandLineToArgvW(cmdline, int *count): the pointer array, then the
// strings, in one block.
void command_line_to_argv_w(X86 *c) {
    uint32_t line = arg(c, 0), count = arg(c, 1);
    if (!count || !gm_valid(count, 4)) {
        set_last_error(87);
        set_eax(c, 0);
        return;
    }
    std::vector<std::string> args = split_command_line(line ? gm_wstr(line) : std::string());
    std::vector<uint32_t> units;
    uint32_t total = 0;
    for (const std::string &a : args) {
        // UTF-8 bytes bound the UTF-16 units from above.
        units.push_back((uint32_t)a.size() + 1);
        total += ((uint32_t)a.size() + 1) * 2;
    }
    uint32_t table = (uint32_t)args.size() * 4 + 4;
    uint32_t block = heap_alloc(table + total, true);
    if (!block) {
        set_last_error(8);
        set_eax(c, 0);
        return;
    }
    uint32_t at = block + table;
    for (size_t i = 0; i < args.size(); ++i) {
        wr32(block + 4 * (uint32_t)i, at);
        uint32_t n = gm_put_wstr(at, args[i], units[i]);
        at += (n + 1) * 2;
    }
    wr32(block + 4 * (uint32_t)args.size(), 0);
    wr32(count, (uint32_t)args.size());
    set_eax(c, block);
}

const ImportShim shims[] = {
    {"KERNEL32.dll", "CompareFileTime", 2, compare_file_time},
    {"KERNEL32.dll", "FormatMessageA", 7, format_message_a},
    {"KERNEL32.dll", "InitializeCriticalSectionAndSpinCount", 2, init_critsec_spin},
    {"KERNEL32.dll", "SetThreadIdealProcessor", 2, set_thread_ideal_processor},
    {"KERNEL32.dll", "InterlockedExchangeAdd", 2, interlocked_exchange_add},
    {"USER32.dll", "EnumDisplayDevicesA", 4, enum_display_devices_a},
    {"USER32.dll", "SetWindowsHookExA", 4, set_windows_hook_ex},
    {"USER32.dll", "GetWindowTextA", 3, empty_text},
    {"USER32.dll", "GetClassNameA", 3, empty_text},
    {"USER32.dll", "GetClassLongA", 2, zero},
    {"USER32.dll", "SwapMouseButton", 1, zero},
    {"SHELL32.dll", "CommandLineToArgvW", 2, command_line_to_argv_w},
    {"ole32.dll", "CoFreeUnusedLibraries", 0, nothing},
    {"COMCTL32.dll", "ImageList_LoadImageA", 7, zero},
    {"COMCTL32.dll", "ImageList_Destroy", 1, one},
    {"d3d9.dll", "D3DPERF_SetOptions", 1, nothing},
};

} // namespace

void compat_2011_register() {
    imports_register(shims, std::size(shims));
}
