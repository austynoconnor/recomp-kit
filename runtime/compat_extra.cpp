// compat_extra.cpp - small KERNEL32, USER32, GDI32 and Winsock entry points a
// 2005-era Direct3D 9 game links and the other shim tables did not cover:
// Star Wars Battlefront II's BattlefrontII.exe was the first to need them.
//
// Each one answers the way Windows XP would on a machine with no network, no
// SIMD the CPUID shim admits to, and no dialog the player could see:
//   - IsProcessorFeaturePresent agrees with recomp_cpuid (cpu.cpp): FPU, TSC
//     and CMOV only, so D3DX and the CRT pick their plain x87 paths.
//   - the file, directory and locale queries report the guest root's view.
//   - dialog boxes are not shown; DialogBoxParamA returns IDOK at once.
//   - Winsock byte-order helpers compute; everything that would touch a
//     socket fails with WSAENETDOWN, so a game's offline path is the one taken.
#include "imports.h"
#include "gdi32_internal.h"
#include "memory.h"
#include "win32.h"
#include "../platform/os.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <string>

namespace {

// --- KERNEL32 ---------------------------------------------------------------

void copy_dir_string(X86 *c, const char *dir) {
    uint32_t buf = arg(c, 0), size = arg(c, 1);
    uint32_t len = (uint32_t)strlen(dir);
    if (!buf || size <= len) {
        set_eax(c, len + 1);
        return;
    }
    memcpy(g_mem + buf, dir, len + 1);
    set_eax(c, len);
}

void get_windows_directory(X86 *c) {
    copy_dir_string(c, "C:\\Windows");
}

// PF_* numbers from winnt.h. Only RDTSC is present: recomp_cpuid reports
// FPU | TSC | CMOV and nothing else, and the two must not disagree, or a
// program that asks both questions takes a path the translator does not cover.
void is_processor_feature_present(X86 *c) {
    uint32_t feature = arg(c, 0);
    set_eax(c, feature == 8 ? 1u : 0u); // PF_RDTSC_INSTRUCTION_AVAILABLE
}

// The CRT asks this of a pipe on stdin; the guest has no pipes.
void peek_named_pipe(X86 *c) {
    set_last_error(6); // ERROR_INVALID_HANDLE
    set_eax(c, 0);
}

void put_filetime(uint32_t at, int64_t unix_seconds) {
    uint64_t ft = (uint64_t)(unix_seconds + 11644473600ll) * 10000000ull;
    wr32(at, (uint32_t)ft);
    wr32(at + 4, (uint32_t)(ft >> 32));
}

// BY_HANDLE_FILE_INFORMATION, 52 bytes: attributes, three FILETIMEs, the
// volume serial, the size (high, low), the link count and the file index.
void get_file_information_by_handle(X86 *c) {
    uint32_t handle = arg(c, 0), out = arg(c, 1);
    std::string path;
    OsStat st{};
    if (!out || !gm_valid(out, 52) || !win32_file_handle_position(handle, &path, nullptr) ||
        os_stat(path.c_str(), &st) != 0) {
        set_last_error(6); // ERROR_INVALID_HANDLE
        set_eax(c, 0);
        return;
    }
    memset(g_mem + out, 0, 52);
    uint32_t attributes = st.is_dir ? 0x10u : 0x20u; // DIRECTORY : ARCHIVE
    if (st.is_readonly)
        attributes |= 0x01u;
    wr32(out, attributes);
    put_filetime(out + 4, st.ctime);
    put_filetime(out + 12, st.atime);
    put_filetime(out + 20, st.mtime);
    wr32(out + 28, 0x1d2c0ffeu); // dwVolumeSerialNumber, any stable value
    wr32(out + 32, (uint32_t)(st.size >> 32));
    wr32(out + 36, (uint32_t)st.size);
    wr32(out + 40, 1); // nNumberOfLinks
    uint32_t index = 2166136261u; // FNV-1a of the path: stable per file
    for (unsigned char ch : path)
        index = (index ^ ch) * 16777619u;
    wr32(out + 48, index);
    set_eax(c, 1);
}

void get_user_default_lang_id(X86 *c) {
    set_eax(c, 0x0409); // en-US, matching GetUserDefaultLCID
}

// GetDateFormatA / GetTimeFormatA for en-US. A SYSTEMTIME is eight WORDs:
// year, month, day of week, day, hour, minute, second, milliseconds. A null
// one means now. The picture language is Windows': d dd ddd dddd, M MM MMM
// MMMM, y yy yyyy, h hh H HH, m mm, s ss, t tt, and 'quoted' text.
struct SysTime {
    int year, month, dow, day, hour, minute, second;
};
SysTime read_systemtime(uint32_t p) {
    SysTime t{};
    if (p && gm_valid(p, 16)) {
        t.year = rd16(p);
        t.month = rd16(p + 2);
        t.dow = rd16(p + 4);
        t.day = rd16(p + 6);
        t.hour = rd16(p + 8);
        t.minute = rd16(p + 10);
        t.second = rd16(p + 12);
        return t;
    }
    struct tm now{};
    os_localtime((int64_t)time(nullptr), &now);
    t = SysTime{now.tm_year + 1900, now.tm_mon + 1, now.tm_wday, now.tm_mday, now.tm_hour, now.tm_min, now.tm_sec};
    return t;
}
std::string format_picture(const std::string &pic, const SysTime &t) {
    static const char *const days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char *const months[] = {"January", "February", "March",     "April",   "May",      "June",
                                         "July",    "August",   "September", "October", "November", "December"};
    std::string out;
    char buf[16];
    for (size_t i = 0; i < pic.size();) {
        char ch = pic[i];
        if (ch == '\'') {
            size_t end = pic.find('\'', i + 1);
            if (end == std::string::npos)
                end = pic.size();
            out += pic.substr(i + 1, end - i - 1);
            i = end + 1;
            continue;
        }
        size_t n = 1;
        while (i + n < pic.size() && pic[i + n] == ch)
            ++n;
        int h12 = t.hour % 12 ? t.hour % 12 : 12;
        switch (ch) {
        case 'd':
            if (n >= 3)
                out += n == 3 ? std::string(days[t.dow % 7], 3) : days[t.dow % 7];
            else
                snprintf(buf, sizeof buf, n == 2 ? "%02d" : "%d", t.day), out += buf;
            break;
        case 'M':
            if (n >= 3)
                out += n == 3 ? std::string(months[(t.month + 11) % 12], 3) : months[(t.month + 11) % 12];
            else
                snprintf(buf, sizeof buf, n == 2 ? "%02d" : "%d", t.month), out += buf;
            break;
        case 'y':
            if (n <= 2)
                snprintf(buf, sizeof buf, n == 2 ? "%02d" : "%d", t.year % 100);
            else
                snprintf(buf, sizeof buf, "%04d", t.year);
            out += buf;
            break;
        case 'h':
            snprintf(buf, sizeof buf, n >= 2 ? "%02d" : "%d", h12), out += buf;
            break;
        case 'H':
            snprintf(buf, sizeof buf, n >= 2 ? "%02d" : "%d", t.hour), out += buf;
            break;
        case 'm':
            snprintf(buf, sizeof buf, n >= 2 ? "%02d" : "%d", t.minute), out += buf;
            break;
        case 's':
            snprintf(buf, sizeof buf, n >= 2 ? "%02d" : "%d", t.second), out += buf;
            break;
        case 't':
            out += n >= 2 ? (t.hour < 12 ? "AM" : "PM") : (t.hour < 12 ? "A" : "P");
            break;
        default:
            out.append(n, ch);
            break;
        }
        i += n;
    }
    return out;
}
// The shared tail: write the text and its NUL, or report the size needed.
void put_formatted(X86 *c, const std::string &text, uint32_t out, uint32_t cch) {
    uint32_t need = (uint32_t)text.size() + 1;
    if (cch == 0) {
        set_eax(c, need);
        return;
    }
    if (!out || cch < need || !gm_valid(out, need)) {
        set_last_error(122); // ERROR_INSUFFICIENT_BUFFER
        set_eax(c, 0);
        return;
    }
    memcpy(g_mem + out, text.c_str(), need);
    set_eax(c, need);
}
// (Locale, dwFlags, lpDate, lpFormat, lpDateStr, cchDate)
void get_date_format(X86 *c) {
    uint32_t flags = arg(c, 1), pic = arg(c, 3);
    std::string picture = pic ? gm_str(pic, 256) : (flags & 2) ? "dddd, MMMM d, yyyy" : "M/d/yyyy"; // DATE_LONGDATE
    put_formatted(c, format_picture(picture, read_systemtime(arg(c, 2))), arg(c, 4), arg(c, 5));
}
// (Locale, dwFlags, lpTime, lpFormat, lpTimeStr, cchTime)
void get_time_format(X86 *c) {
    uint32_t flags = arg(c, 1), pic = arg(c, 3);
    std::string picture;
    if (pic) {
        picture = gm_str(pic, 256);
    } else {
        picture = (flags & 8) ? "H:mm" : "h:mm"; // TIME_FORCE24HOURFORMAT
        if (!(flags & 2))                        // TIME_NOSECONDS
            picture += ":ss";
        if (!(flags & 8) && !(flags & 4)) // TIME_NOTIMEMARKER
            picture += " tt";
    }
    put_formatted(c, format_picture(picture, read_systemtime(arg(c, 2))), arg(c, 4), arg(c, 5));
}

// --- USER32 -----------------------------------------------------------------

// No modal dialog can be shown or answered. A game's message box or server
// panel gets IDOK as though the player had dismissed it.
void dialog_box_param(X86 *c) {
    uint32_t tmpl = arg(c, 1);
    char key[48];
    snprintf(key, sizeof key, "DialogBoxParamA:%08x", tmpl);
    log_once(key, "DialogBoxParamA(template %s%u): no dialog is shown; returning IDOK",
             tmpl < 0x10000 ? "#" : "@", tmpl);
    set_eax(c, 1); // IDOK
}
void get_dlg_item(X86 *c) {
    set_eax(c, 0);
}
void end_dialog(X86 *c) {
    set_eax(c, 1);
}

// A US keyboard layout's character for a virtual key, or 0. `state` is the
// 256-byte key-state array ToAscii and ToUnicode are given.
uint32_t key_char(uint32_t vk, uint32_t state) {
    auto down = [&](uint32_t k) { return state && gm_valid(state + k, 1) && (g_mem[state + k] & 0x80); };
    bool shift = down(0x10) || down(0xa0) || down(0xa1);
    bool caps = state && gm_valid(state + 0x14, 1) && (g_mem[state + 0x14] & 1);
    if (down(0x11) || down(0x12))
        return 0; // Ctrl or Alt: no character on a US layout
    if (vk >= 'A' && vk <= 'Z')
        return (shift != caps) ? vk : vk + 32;
    if (vk >= '0' && vk <= '9')
        return shift ? (uint32_t)")!@#$%^&*("[vk - '0'] : vk;
    if (vk >= 0x60 && vk <= 0x69)
        return '0' + (vk - 0x60); // numeric keypad
    switch (vk) {
    case 0x08: return 8;
    case 0x09: return 9;
    case 0x0d: return 13;
    case 0x1b: return 27;
    case 0x20: return ' ';
    case 0x6a: return '*';
    case 0x6b: return '+';
    case 0x6d: return '-';
    case 0x6e: return '.';
    case 0x6f: return '/';
    case 0xba: return shift ? ':' : ';';
    case 0xbb: return shift ? '+' : '=';
    case 0xbc: return shift ? '<' : ',';
    case 0xbd: return shift ? '_' : '-';
    case 0xbe: return shift ? '>' : '.';
    case 0xbf: return shift ? '?' : '/';
    case 0xc0: return shift ? '~' : '`';
    case 0xdb: return shift ? '{' : '[';
    case 0xdc: return shift ? '|' : '\\';
    case 0xdd: return shift ? '}' : ']';
    case 0xde: return shift ? '"' : '\'';
    default: return 0;
    }
}

// ToAscii(vk, scan, state, LPWORD out, flags)
void to_ascii(X86 *c) {
    uint32_t ch = key_char(arg(c, 0), arg(c, 2)), out = arg(c, 3);
    if (!ch || !out || !gm_valid(out, 2)) {
        set_eax(c, 0);
        return;
    }
    wr16(out, (uint16_t)ch);
    set_eax(c, 1);
}

// ToUnicode(vk, scan, state, LPWSTR out, int cch, flags)
void to_unicode(X86 *c) {
    uint32_t ch = key_char(arg(c, 0), arg(c, 2)), out = arg(c, 3), cch = arg(c, 4);
    if (!ch || !out || (int32_t)cch < 1 || !gm_valid(out, 2)) {
        set_eax(c, 0);
        return;
    }
    wr16(out, (uint16_t)ch);
    if (cch > 1 && gm_valid(out + 2, 2))
        wr16(out + 2, 0);
    set_eax(c, 1);
}

// DrawTextA: widen the ANSI text (code page 1252 is Latin-1 for the bytes a
// game's debug and message text uses) and draw it with the wide path.
void draw_text_a(X86 *c) {
    uint32_t hdc = arg(c, 0), text = arg(c, 1), count = arg(c, 2), rect = arg(c, 3), flags = arg(c, 4);
    if (!text || !rect) {
        set_eax(c, 0);
        return;
    }
    std::string s = (int32_t)count < 0 ? gm_str(text) : std::string((const char *)g_mem + text, count);
    uint32_t wide = heap_alloc((uint32_t)(s.size() + 1) * 2, true);
    if (!wide) {
        set_eax(c, 0);
        return;
    }
    for (size_t i = 0; i < s.size(); ++i)
        wr16(wide + (uint32_t)i * 2, (uint8_t)s[i]);
    set_eax(c, gdi::draw_text(hdc, wide, (uint32_t)s.size(), rect, flags));
    heap_free(wide);
}

// --- GDI32 ------------------------------------------------------------------

void set_map_mode(X86 *c) {
    set_eax(c, 1); // the previous mode, MM_TEXT: the only one the runtime draws in
}
void set_icm_mode(X86 *c) {
    set_eax(c, 1); // ICM_OFF before and after
}
// An identity ramp: three channels of 256 WORDs, 0x0000..0xff00.
void get_device_gamma_ramp(X86 *c) {
    uint32_t ramp = arg(c, 1);
    if (!ramp || !gm_valid(ramp, 3 * 256 * 2)) {
        set_eax(c, 0);
        return;
    }
    for (uint32_t ch = 0; ch < 3; ++ch)
        for (uint32_t i = 0; i < 256; ++i)
            wr16(ramp + (ch * 256 + i) * 2, (uint16_t)(i << 8));
    set_eax(c, 1);
}

// --- Winsock ----------------------------------------------------------------
// Byte order on a little-endian guest. inet_addr parses a dotted quad.

constexpr uint32_t SOCKET_ERROR_ = 0xffffffffu, WSAENETDOWN_ = 10050;
uint32_t g_wsa_error = 0;

void htons_(X86 *c) {
    uint32_t v = arg(c, 0) & 0xffff;
    set_eax(c, ((v & 0xff) << 8) | (v >> 8));
}
void htonl_(X86 *c) {
    uint32_t v = arg(c, 0);
    set_eax(c, (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24));
}
void inet_addr_(X86 *c) {
    std::string s = arg(c, 0) ? gm_str(arg(c, 0), 64) : std::string();
    unsigned a, b, d, e;
    char tail;
    if (sscanf(s.c_str(), "%u.%u.%u.%u%c", &a, &b, &d, &e, &tail) != 4 || a > 255 || b > 255 || d > 255 ||
        e > 255) {
        set_eax(c, 0xffffffffu); // INADDR_NONE
        return;
    }
    set_eax(c, a | (b << 8) | (d << 16) | (e << 24));
}
void inet_ntoa_(X86 *c) {
    static uint32_t buf = 0;
    if (!buf || !heap_owns(buf))
        buf = heap_alloc(32, true);
    uint32_t addr = arg(c, 0);
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%u.%u.%u.%u", addr & 0xff, (addr >> 8) & 0xff, (addr >> 16) & 0xff,
             (addr >> 24) & 0xff);
    if (buf)
        gm_put_str(buf, tmp, 32);
    set_eax(c, buf);
}
void net_down(X86 *c) {
    g_wsa_error = WSAENETDOWN_;
    set_eax(c, SOCKET_ERROR_);
}
void fd_is_set(X86 *c) {
    set_eax(c, 0);
}
void wsa_ok(X86 *c) {
    set_eax(c, 0);
}
void wsa_get_last_error(X86 *c) {
    set_eax(c, g_wsa_error);
}
void null_result(X86 *c) {
    g_wsa_error = WSAENETDOWN_;
    set_eax(c, 0);
}
void ws2_startup(X86 *c) {
    uint32_t version = arg(c, 0), data = arg(c, 1);
    if (data && gm_valid(data, 400)) {
        memset(g_mem + data, 0, 400);
        wr16(data, (uint16_t)version);
        wr16(data + 2, 0x0202);
        gm_put_str(data + 4, "recomp sockets (offline)", 257);
        gm_put_str(data + 4 + 257, "Network is down", 129);
    }
    g_wsa_error = 0;
    set_eax(c, 0);
}
const ImportShim shims[] = {
    {"KERNEL32.dll", "GetWindowsDirectoryA", 2, get_windows_directory},
    {"KERNEL32.dll", "IsProcessorFeaturePresent", 1, is_processor_feature_present},
    {"KERNEL32.dll", "PeekNamedPipe", 6, peek_named_pipe},
    {"KERNEL32.dll", "GetFileInformationByHandle", 2, get_file_information_by_handle},
    {"KERNEL32.dll", "GetUserDefaultLangID", 0, get_user_default_lang_id},
    {"KERNEL32.dll", "GetSystemDefaultLangID", 0, get_user_default_lang_id},
    {"KERNEL32.dll", "GetDateFormatA", 6, get_date_format},
    {"KERNEL32.dll", "GetTimeFormatA", 6, get_time_format},
    {"USER32.dll", "DialogBoxParamA", 5, dialog_box_param},
    {"USER32.dll", "GetDlgItem", 2, get_dlg_item},
    {"USER32.dll", "EndDialog", 2, end_dialog},
    {"USER32.dll", "ToAscii", 5, to_ascii},
    {"USER32.dll", "ToUnicode", 6, to_unicode},
    {"USER32.dll", "DrawTextA", 5, draw_text_a},
    {"GDI32.dll", "SetMapMode", 2, set_map_mode},
    {"GDI32.dll", "SetICMMode", 2, set_icm_mode},
    {"GDI32.dll", "GetDeviceGammaRamp", 2, get_device_gamma_ramp},
    // Winsock 2 and 1.1 by name. The byte-order helpers are real; the rest
    // report a network that is down.
    {"WS2_32.dll", "htons", 1, htons_},
    {"WS2_32.dll", "ntohs", 1, htons_},
    {"WS2_32.dll", "htonl", 1, htonl_},
    {"WS2_32.dll", "ntohl", 1, htonl_},
    {"WS2_32.dll", "inet_addr", 1, inet_addr_},
    {"WS2_32.dll", "inet_ntoa", 1, inet_ntoa_},
    {"WS2_32.dll", "WSAStartup", 2, ws2_startup},
    {"WS2_32.dll", "WSACleanup", 0, wsa_ok},
    {"WS2_32.dll", "WSAGetLastError", 0, wsa_get_last_error},
    {"WS2_32.dll", "socket", 3, net_down},
    {"WS2_32.dll", "bind", 3, net_down},
    {"WS2_32.dll", "connect", 3, net_down},
    {"WS2_32.dll", "listen", 2, net_down},
    {"WS2_32.dll", "accept", 3, net_down},
    {"WS2_32.dll", "send", 4, net_down},
    {"WS2_32.dll", "recv", 4, net_down},
    {"WS2_32.dll", "sendto", 6, net_down},
    {"WS2_32.dll", "recvfrom", 6, net_down},
    {"WS2_32.dll", "closesocket", 1, wsa_ok},
    {"WS2_32.dll", "ioctlsocket", 3, net_down},
    {"WS2_32.dll", "setsockopt", 5, net_down},
    {"WS2_32.dll", "gethostbyname", 1, null_result},
    {"WSOCK32.dll", "htonl", 1, htonl_},
    {"WSOCK32.dll", "ntohl", 1, htonl_},
    {"WSOCK32.dll", "htons", 1, htons_},
    {"WSOCK32.dll", "ntohs", 1, htons_},
    {"WSOCK32.dll", "select", 5, net_down},
    {"WSOCK32.dll", "__WSAFDIsSet", 2, fd_is_set},
    {"WSOCK32.dll", "getsockopt", 5, net_down},
    {"WSOCK32.dll", "getsockname", 3, net_down},
    {"WSOCK32.dll", "shutdown", 2, net_down},
};

} // namespace

void compat_extra_register() {
    imports_register(shims, sizeof shims / sizeof shims[0]);
}
