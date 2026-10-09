// wmi.cpp - the smallest WMI a game's XInput check needs.
//
// DirectInput lists an Xbox 360 pad as a joystick too, so games that read
// pads through XInput first ask which DirectInput joysticks are XInput pads,
// to leave those to XInput. Almost all of them use the DirectX SDK's
// IsXInputDevice sample: connect to WMI's root\cimv2, enumerate
// Win32_PNPEntity, and for each DeviceID that contains "IG_" compare its
// VID_xxxx and PID_xxxx with the joystick's product GUID. Without WMI the
// answer is "no XInput pads", and a game like Bully: Scholarship Edition
// then never calls XInput at all.
//
// So this answers exactly that question: CLSID_WbemLocator connects to any
// namespace, and an enumeration of Win32_PNPEntity holds one device, the
// kit's pad, with an Xbox 360 controller's DeviceID - but only while the
// host serves the pad through XInput (dinput_joystick.cpp then gives the
// DirectInput view the matching product GUID). Every other class, and every
// other question, is an empty answer.
#include "com.h"
#include "dx.h"
#include "host_api.h"
#include "../runtime/guest.h"
#include "../runtime/memory.h"

#include <iterator>
#include <string.h>

namespace {

// {4590f811-1d3a-11d0-891f-00aa004b2e24} and IID_IWbemLocator
// {dc12a687-737f-11cf-884d-00aa004b2e24}.
const uint8_t CLSID_WbemLocator_[16] = {0x11, 0xf8, 0x90, 0x45, 0x3a, 0x1d, 0xd0, 0x11,
                                        0x89, 0x1f, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24};
const uint8_t IID_IWbemLocator_[16] = {0x87, 0xa6, 0x12, 0xdc, 0x7f, 0x73, 0xcf, 0x11,
                                       0x88, 0x4d, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24};

const uint32_t S_FALSE_ = 1;
const uint32_t E_NOTIMPL_ = 0x80004001u;
const uint32_t WBEM_E_NOT_FOUND = 0x80041002u;
const uint32_t WBEM_E_INVALID_PARAMETER = 0x80041008u;
const uint16_t VT_BSTR_ = 8;
const uint32_t CIM_STRING = 8;

// What a wired Xbox 360 controller reports, VID 045E PID 028E.
const char kPadDeviceId[] = "USB\\VID_045E&PID_028E&IG_00\\6&1A2B3C4D&0&00";

bool pad_listed() {
    return wmi_pad_is_xinput();
}

// A BSTR of an ASCII string: length prefix, UTF-16 data, NUL.
uint32_t bstr_of(const char *s) {
    uint32_t n = (uint32_t)strlen(s);
    uint32_t p = heap_alloc(n * 2 + 6, true);
    if (!p)
        return 0;
    wr32(p, n * 2);
    for (uint32_t i = 0; i < n; ++i)
        wr16(p + 4 + 2 * i, (uint8_t)s[i]);
    return p + 4;
}

// A guest wide string compared with an ASCII one, ignoring case.
bool wide_is(uint32_t w, const char *s) {
    if (!w)
        return false;
    for (uint32_t i = 0;; ++i) {
        if (!gm_valid(w + 2 * i, 2))
            return false;
        uint16_t a = rd16(w + 2 * i);
        char b = s[i];
        if (a < 128 && b)
            if ((a | 0x20) == (uint16_t)(b | 0x20) || a == (uint16_t)b)
                continue;
        return a == 0 && b == 0;
    }
}

void put(uint32_t p, uint32_t v) {
    if (p && gm_valid(p, 4))
        wr32(p, v);
}

// Hands out a new object through `out` as `iface`.
uint32_t make(ComIface iface, uint32_t out, uint32_t listed = 0) {
    if (!out || !gm_valid(out, 4))
        return WBEM_E_INVALID_PARAMETER;
    ComObj *o = com_new(K_WMI);
    uint32_t view = o ? com_view(o, iface) : 0;
    if (o)
        o->wmi_left = listed;
    wr32(out, view);
    return view ? S_OK : E_OUTOFMEMORY;
}

void not_implemented(X86 *c) {
    set_eax(c, E_NOTIMPL_);
}

ComObj *create_locator() {
    return com_new(K_WMI);
}

// IWbemLocator::ConnectServer(this, network, user, password, locale,
// securityFlags, authority, ctx, ppNamespace).
void L_ConnectServer(X86 *c) {
    set_eax(c, make(IF_WBEM_SERVICES, arg(c, 8)));
}

// IWbemServices::CreateInstanceEnum(this, class, flags, ctx, ppEnum).
void S_CreateInstanceEnum(X86 *c) {
    uint32_t listed = wide_is(arg(c, 1), "Win32_PNPEntity") && pad_listed() ? 1 : 0;
    set_eax(c, make(IF_WBEM_ENUM, arg(c, 4), listed));
}
// ExecQuery(this, language, query, flags, ctx, ppEnum): an empty result.
void S_ExecQuery(X86 *c) {
    set_eax(c, make(IF_WBEM_ENUM, arg(c, 5)));
}

// IEnumWbemClassObject::Next(this, timeout, count, apObjects, puReturned).
void E_Next(X86 *c) {
    ComObj *o = com_this_arg(c);
    uint32_t count = arg(c, 2), objects = arg(c, 3), returned = arg(c, 4);
    if (!o || !objects || !returned) {
        set_eax(c, WBEM_E_INVALID_PARAMETER);
        return;
    }
    uint32_t n = 0;
    while (n < count && o->wmi_left && gm_valid(objects + 4 * n, 4)) {
        if (make(IF_WBEM_OBJECT, objects + 4 * n) != S_OK)
            break;
        --o->wmi_left;
        ++n;
    }
    put(returned, n);
    set_eax(c, n == count ? S_OK : S_FALSE_);
}
void E_Reset(X86 *c) {
    set_eax(c, S_OK);
}

// IWbemClassObject::Get(this, name, flags, pVal, pType, plFlavor).
void O_Get(X86 *c) {
    uint32_t value = arg(c, 3);
    if (!wide_is(arg(c, 1), "DeviceID")) {
        set_eax(c, WBEM_E_NOT_FOUND);
        return;
    }
    if (value && gm_valid(value, 16)) {
        gm_zero(value, 16);
        wr16(value, VT_BSTR_);
        wr32(value + 8, bstr_of(kPadDeviceId));
    }
    put(arg(c, 4), CIM_STRING);
    put(arg(c, 5), 0);
    set_eax(c, S_OK);
}

#define IUNKNOWN                                                                                   \
    {"QueryInterface", 3, com_QueryInterface}, {"AddRef", 1, com_AddRef}, {                        \
        "Release", 1, com_Release                                                                  \
    }

const ComMethod g_locator[] = {
    IUNKNOWN,
    {"ConnectServer", 9, L_ConnectServer},
};
const ComMethod g_services[] = {
    IUNKNOWN,
    {"OpenNamespace", 6, not_implemented},
    {"CancelAsyncCall", 2, not_implemented},
    {"QueryObjectSink", 3, not_implemented},
    {"GetObject", 6, not_implemented},
    {"GetObjectAsync", 5, not_implemented},
    {"PutClass", 5, not_implemented},
    {"PutClassAsync", 5, not_implemented},
    {"DeleteClass", 5, not_implemented},
    {"DeleteClassAsync", 5, not_implemented},
    {"CreateClassEnum", 5, not_implemented},
    {"CreateClassEnumAsync", 5, not_implemented},
    {"PutInstance", 5, not_implemented},
    {"PutInstanceAsync", 5, not_implemented},
    {"DeleteInstance", 5, not_implemented},
    {"DeleteInstanceAsync", 5, not_implemented},
    {"CreateInstanceEnum", 5, S_CreateInstanceEnum},
    {"CreateInstanceEnumAsync", 5, not_implemented},
    {"ExecQuery", 6, S_ExecQuery},
    {"ExecQueryAsync", 6, not_implemented},
    {"ExecNotificationQuery", 6, not_implemented},
    {"ExecNotificationQueryAsync", 6, not_implemented},
    {"ExecMethod", 8, not_implemented},
    {"ExecMethodAsync", 8, not_implemented},
};
const ComMethod g_enum[] = {
    IUNKNOWN,
    {"Reset", 1, E_Reset},
    {"Next", 5, E_Next},
    {"NextAsync", 3, not_implemented},
    {"Clone", 2, not_implemented},
    {"Skip", 3, not_implemented},
};
const ComMethod g_object[] = {
    IUNKNOWN,
    {"GetQualifierSet", 2, not_implemented},
    {"Get", 6, O_Get},
    {"Put", 5, not_implemented},
    {"Delete", 2, not_implemented},
    {"GetNames", 5, not_implemented},
    {"BeginEnumeration", 2, not_implemented},
    {"Next", 6, not_implemented},
    {"EndEnumeration", 1, not_implemented},
    {"GetPropertyQualifierSet", 3, not_implemented},
    {"Clone", 2, not_implemented},
    {"GetObjectText", 3, not_implemented},
    {"SpawnDerivedClass", 3, not_implemented},
    {"SpawnInstance", 3, not_implemented},
    {"CompareTo", 3, not_implemented},
    {"GetPropertyOrigin", 3, not_implemented},
    {"InheritsFrom", 2, not_implemented},
    {"GetMethod", 5, not_implemented},
    {"PutMethod", 5, not_implemented},
    {"DeleteMethod", 2, not_implemented},
    {"BeginMethodEnumeration", 2, not_implemented},
    {"NextMethod", 5, not_implemented},
    {"EndMethodEnumeration", 1, not_implemented},
    {"GetMethodQualifierSet", 3, not_implemented},
    {"GetMethodOrigin", 3, not_implemented},
};
#undef IUNKNOWN

} // namespace

void wmi_register() {
    static bool done = false;
    if (done)
        return;
    done = true;
    com_define(IF_WBEM_LOCATOR, "wbemprox.dll", "IWbemLocator", g_locator, std::size(g_locator));
    com_define(IF_WBEM_SERVICES, "wbemprox.dll", "IWbemServices", g_services,
               std::size(g_services));
    com_define(IF_WBEM_ENUM, "wbemprox.dll", "IEnumWbemClassObject", g_enum, std::size(g_enum));
    com_define(IF_WBEM_OBJECT, "wbemprox.dll", "IWbemClassObject", g_object, std::size(g_object));
    for (ComIface i : {IF_WBEM_LOCATOR, IF_WBEM_SERVICES, IF_WBEM_ENUM, IF_WBEM_OBJECT})
        com_bind(i, K_WMI);
    com_register_iid(IF_WBEM_LOCATOR, IID_IWbemLocator_);
    com_register_class(CLSID_WbemLocator_, "WbemLocator", IF_WBEM_LOCATOR, create_locator);
}
