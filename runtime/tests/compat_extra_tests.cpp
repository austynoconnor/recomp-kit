// The small KERNEL32, USER32, GDI32 and Winsock shims in compat_extra.cpp, and
// the two GDI32 and Bink additions beside them; no game image required.
#include "../imports.h"
#include "../loader.h"
#include "../memory.h"
#include "../win32.h"
#include "../../platform/os.h"
#include "../../dx/dx.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_checks = 0, g_failures = 0;
static bool check(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static bool check(bool ok, const char *fmt, ...) {
    ++g_checks;
    va_list ap;
    va_start(ap, fmt);
    char msg[512];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (!ok)
        ++g_failures;
    printf("  [%s] %s\n", ok ? "ok" : "FAIL", msg);
    return ok;
}

// Pushes the arguments and a return address, dispatches, and checks the
// shim's argument count left ESP where a stdcall caller expects it.
static uint32_t call_import(X86 *c, const char *dll, const char *name, const std::vector<uint32_t> &args) {
    uint32_t tramp = imports_resolve(dll, name);
    if (!check(tramp != 0, "%s!%s is registered", dll, name))
        return 0;
    uint32_t esp = c->r[R_ESP], before = esp;
    for (size_t i = args.size(); i-- > 0;) {
        esp -= 4;
        wr32(esp, args[i]);
    }
    esp -= 4;
    wr32(esp, 0);
    c->r[R_ESP] = esp;
    imports_dispatch(c, tramp);
    check(c->r[R_ESP] == before, "%s!%s pops %zu arguments", dll, name, args.size());
    c->r[R_ESP] = before;
    return c->r[R_EAX];
}

static void test_kernel32(X86 *c, uint32_t s) {
    // Only RDTSC (8): no SSE (6, 10), no 3DNow! (7), no MMX (3), no CX8 (2).
    check(call_import(c, "KERNEL32.dll", "IsProcessorFeaturePresent", {8}) == 1, "RDTSC present");
    for (uint32_t f : {2u, 3u, 6u, 7u, 10u, 13u})
        check(call_import(c, "KERNEL32.dll", "IsProcessorFeaturePresent", {f}) == 0, "feature %u absent", f);
    uint32_t n = call_import(c, "KERNEL32.dll", "GetWindowsDirectoryA", {s, 64});
    check(n == 10 && gm_str(s) == "C:\\Windows", "GetWindowsDirectoryA -> %s (%u)", gm_str(s).c_str(), n);
    check(call_import(c, "KERNEL32.dll", "GetWindowsDirectoryA", {s, 4}) == 11, "short buffer reports the size");
    check(call_import(c, "KERNEL32.dll", "GetUserDefaultLangID", {}) == 0x0409, "en-US language");
    check(call_import(c, "KERNEL32.dll", "PeekNamedPipe", {0, 0, 0, 0, 0, 0}) == 0, "no pipes");
    check(call_import(c, "KERNEL32.dll", "GetFileInformationByHandle", {0x1234, s}) == 0,
          "an unknown handle has no file information");

    // 2006-03-07 14:05:09, a Tuesday.
    uint32_t st = s + 0x100, out = s + 0x200, pic = s + 0x300;
    const uint16_t fields[8] = {2006, 3, 2, 7, 14, 5, 9, 0};
    for (int i = 0; i < 8; ++i)
        wr16(st + i * 2, fields[i]);
    n = call_import(c, "KERNEL32.dll", "GetDateFormatA", {0x409, 0, st, 0, out, 64});
    check(gm_str(out) == "3/7/2006" && n == 9, "short date: %s", gm_str(out).c_str());
    n = call_import(c, "KERNEL32.dll", "GetDateFormatA", {0x409, 2, st, 0, out, 64});
    check(gm_str(out) == "Tuesday, March 7, 2006", "long date: %s", gm_str(out).c_str());
    gm_put_str(pic, "yyyy'-'MM'-'dd ddd MMM yy", 64);
    call_import(c, "KERNEL32.dll", "GetDateFormatA", {0x409, 0, st, pic, out, 64});
    check(gm_str(out) == "2006-03-07 Tue Mar 06", "picture date: %s", gm_str(out).c_str());
    n = call_import(c, "KERNEL32.dll", "GetTimeFormatA", {0x409, 0, st, 0, out, 64});
    check(gm_str(out) == "2:05:09 PM" && n == 11, "time: %s", gm_str(out).c_str());
    call_import(c, "KERNEL32.dll", "GetTimeFormatA", {0x409, 8 | 2, st, 0, out, 64});
    check(gm_str(out) == "14:05", "24-hour time without seconds: %s", gm_str(out).c_str());
    check(call_import(c, "KERNEL32.dll", "GetTimeFormatA", {0x409, 0, st, 0, out, 0}) == 11,
          "a zero-length buffer asks for the size");
    check(call_import(c, "KERNEL32.dll", "GetTimeFormatA", {0x409, 0, st, 0, out, 3}) == 0,
          "a short buffer fails");
}

static void test_user32(X86 *c, uint32_t s) {
    uint32_t state = s, out = s + 0x200;
    memset(g_mem + state, 0, 256);
    check(call_import(c, "USER32.dll", "ToAscii", {'A', 0x1e, state, out, 0}) == 1 && rd16(out) == 'a',
          "ToAscii A -> %c", (char)rd16(out));
    g_mem[state + 0x10] = 0x80; // Shift held
    check(call_import(c, "USER32.dll", "ToAscii", {'A', 0x1e, state, out, 0}) == 1 && rd16(out) == 'A',
          "Shift+A -> %c", (char)rd16(out));
    check(call_import(c, "USER32.dll", "ToAscii", {'1', 0x02, state, out, 0}) == 1 && rd16(out) == '!',
          "Shift+1 -> %c", (char)rd16(out));
    g_mem[state + 0x10] = 0;
    check(call_import(c, "USER32.dll", "ToUnicode", {0x20, 0x39, state, out, 4, 0}) == 1 && rd16(out) == ' ',
          "ToUnicode space");
    check(call_import(c, "USER32.dll", "ToAscii", {0x70, 0x3b, state, out, 0}) == 0, "F1 has no character");
    check(call_import(c, "USER32.dll", "DialogBoxParamA", {IMAGE_BASE, 101, 0, 0, 0}) == 1, "dialogs say IDOK");
    check(call_import(c, "USER32.dll", "EndDialog", {0, 1}) == 1, "EndDialog");
    check(call_import(c, "USER32.dll", "GetDlgItem", {0, 1000}) == 0, "no dialog items");
}

static void test_gdi32(X86 *c, uint32_t s) {
    uint32_t ramp = s;
    check(call_import(c, "GDI32.dll", "GetDeviceGammaRamp", {0, ramp}) == 1, "GetDeviceGammaRamp");
    check(rd16(ramp) == 0 && rd16(ramp + 255 * 2) == 0xff00 && rd16(ramp + (512 + 128) * 2) == 0x8000,
          "identity ramp on all three channels");
    check(call_import(c, "GDI32.dll", "SetICMMode", {0, 1}) == 1, "SetICMMode");
    uint32_t lf = s + 0x800;
    memset(g_mem + lf, 0, 60);
    wr32(lf, (uint32_t)-16);
    gm_put_str(lf + 28, "Arial", 32);
    check(call_import(c, "GDI32.dll", "CreateFontIndirectA", {lf}) != 0, "CreateFontIndirectA makes a font");
}

static void test_winsock(X86 *c, uint32_t s) {
    check(call_import(c, "WS2_32.dll", "htons", {0x1234}) == 0x3412, "htons");
    check(call_import(c, "WS2_32.dll", "ntohl", {0x12345678}) == 0x78563412, "ntohl");
    check(call_import(c, "WSOCK32.dll", "htonl", {0x01020304}) == 0x04030201, "WSOCK32 htonl");
    gm_put_str(s, "192.168.1.20", 32);
    uint32_t addr = call_import(c, "WS2_32.dll", "inet_addr", {s});
    check(addr == 0x1401a8c0u, "inet_addr -> %08x", addr);
    gm_put_str(s, "not.an.address", 32);
    check(call_import(c, "WS2_32.dll", "inet_addr", {s}) == 0xffffffffu, "inet_addr rejects a name");
    uint32_t text = call_import(c, "WS2_32.dll", "inet_ntoa", {0x1401a8c0u});
    check(text && gm_str(text) == "192.168.1.20", "inet_ntoa");
    check(call_import(c, "WS2_32.dll", "WSAStartup", {0x0202, s + 0x100}) == 0, "WSAStartup succeeds");
    check(call_import(c, "WS2_32.dll", "socket", {2, 2, 17}) == 0xffffffffu, "socket fails");
    check(call_import(c, "WS2_32.dll", "WSAGetLastError", {}) == 10050, "with WSAENETDOWN");
    check(call_import(c, "WSOCK32.dll", "select", {0, 0, 0, 0, 0}) == 0xffffffffu, "select fails");
    check(call_import(c, "WS2_32.dll", "gethostbyname", {s}) == 0, "no name resolution");
}

// The Microsoft Layer for Unicode finds GetFileAttributesW by walking
// kernel32's export directory itself, from GetModuleHandleA's handle.
static void test_pseudo_module_exports(X86 *c, uint32_t s) {
    gm_put_str(s, "kernel32.dll", 32);
    uint32_t h = call_import(c, "KERNEL32.dll", "GetModuleHandleA", {s});
    check(h && (h & 0xffff) == 0 && h < GUEST_SIZE, "kernel32 handle %08x is 64 KB aligned guest memory", h);
    if (!h || h >= GUEST_SIZE)
        return;
    check(rd16(h) == 0x5a4d, "MZ");
    uint32_t nt = h + rd32(h + 0x3c);
    check(rd32(nt) == 0x4550 && rd16(nt + 4) == 0x14c, "PE header for i386");
    check(rd32(nt + 0x74) >= 1, "has a data directory");
    uint32_t dir = h + rd32(nt + 0x78);
    uint32_t n = rd32(dir + 24), names = h + rd32(dir + 32), ords = h + rd32(dir + 36),
             funcs = h + rd32(dir + 28);
    check(gm_str(h + rd32(dir + 12)) == "kernel32.dll", "export name %s", gm_str(h + rd32(dir + 12)).c_str());
    check(n > 100, "%u named exports", n);
    bool sorted = true;
    uint32_t found = 0;
    for (uint32_t i = 0; i < n; ++i) {
        std::string nm = gm_str(h + rd32(names + 4 * i));
        if (i && gm_str(h + rd32(names + 4 * (i - 1))) >= nm)
            sorted = false;
        if (nm == "GetFileAttributesW")
            found = h + rd32(funcs + 4 * rd16(ords + 2 * i)); // wraps like the guest's ADD
    }
    check(sorted, "names are in byte order");
    gm_put_str(s, "GetFileAttributesW", 32);
    uint32_t gpa = call_import(c, "KERNEL32.dll", "GetProcAddress", {h, s});
    check(found && found == gpa, "walked GetFileAttributesW %08x matches GetProcAddress %08x", found, gpa);
    gm_put_str(s, "KERNEL32", 32);
    check(call_import(c, "KERNEL32.dll", "GetModuleHandleA", {s}) == h, "same handle by another spelling");
}

static void test_bink(X86 *c) {
    // Without a D3D9 surface the type is unknown; the hooks are accepted.
    check(call_import(c, "binkw32.dll", "_BinkDX9SurfaceType@4", {0}) == 0, "BinkDX9SurfaceType(null)");
    check(call_import(c, "binkw32.dll", "_BinkSetIO@4", {0x00401000}) == 0, "BinkSetIO");
    check(call_import(c, "binkw32.dll", "_BinkSetIOSize@4", {0x10000}) == 0, "BinkSetIOSize");
    check(call_import(c, "binkw32.dll", "_BinkSetMemory@8", {0x00401000, 0x00401010}) == 0, "BinkSetMemory");
    check(call_import(c, "binkw32.dll", "_BinkSetVolume@12", {0, 0, 32768}) == 0, "BinkSetVolume");
}

int main() {
    mem_init();
    imports_init();
    bink_register();
    X86 c;
    loader_init_context(&c);
    uint32_t s = heap_alloc(0x4000, true);
    test_kernel32(&c, s);
    test_user32(&c, s);
    test_gdi32(&c, s);
    test_winsock(&c, s);
    test_pseudo_module_exports(&c, s);
    test_bink(&c);
    printf("%d checks, %d failures\n", g_checks, g_failures);
    mem_shutdown();
    return g_failures ? 1 : 0;
}
