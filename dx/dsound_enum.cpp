// dsound_enum.cpp - DirectSoundEnumerateA/W: the one output device.
//
// A game lists devices to fill its sound settings and to pick the GUID it
// passes to DirectSoundCreate. The runtime has exactly one output, presented
// the way Windows presents the default device: a NULL GUID named "Primary
// Sound Driver" with an empty module name. The callback is
// BOOL CALLBACK (LPGUID, LPCSTR description, LPCSTR module, LPVOID context);
// it is called once, and its answer does not matter because there is nothing
// after it. The names live in a small guest block allocated on first use.
//
// DirectSoundEnumerateA is DSOUND ordinal 2 and DirectSoundEnumerateW ordinal
// 3; both names are registered, since the loader names ordinal imports "ordN".
#include "dx.h"
#include "../runtime/guest.h"
#include "../runtime/imports.h"
#include "../runtime/memory.h"

#include <iterator>

namespace {

uint32_t g_names = 0; // "Primary Sound Driver\0\0" (ANSI) then the UTF-16 copy

uint32_t names_block() {
    if (!g_names) {
        g_names = heap_alloc(128, true);
        if (g_names) {
            gm_put_str(g_names, "Primary Sound Driver", 32);
            wr8(g_names + 32, 0); // empty module name
            gm_put_wstr(g_names + 40, "Primary Sound Driver", 32);
            wr16(g_names + 120, 0);
        }
    }
    return g_names;
}

void enumerate(X86 *c, bool wide) {
    uint32_t cb = arg(c, 0), ctx = arg(c, 1), names = names_block();
    if (!cb || !names) {
        set_eax(c, 0x80070057u); // DSERR_INVALIDPARAM
        return;
    }
    if (wide)
        guest_call(c, cb, 0, names + 40, names + 120, ctx);
    else
        guest_call(c, cb, 0, names, names + 32, ctx);
    set_eax(c, 0); // DS_OK
}

void DirectSoundEnumerateA(X86 *c) {
    enumerate(c, false);
}
void DirectSoundEnumerateW(X86 *c) {
    enumerate(c, true);
}

const ImportShim g_dsound_enum_shims[] = {
    {"DSOUND.dll", "ord2", 2, DirectSoundEnumerateA},
    {"DSOUND.dll", "DirectSoundEnumerateA", 2, DirectSoundEnumerateA},
    {"DSOUND.dll", "ord3", 2, DirectSoundEnumerateW},
    {"DSOUND.dll", "DirectSoundEnumerateW", 2, DirectSoundEnumerateW},
};

} // namespace

void dsound_enum_register() {
    g_names = 0; // the guest heap was reset with everything else
    imports_register(g_dsound_enum_shims, std::size(g_dsound_enum_shims));
}
