#include "d3d9_assemble.h"
#include "com.h"
#include "../runtime/memory.h"
#include <cstring>
#include <iterator>

namespace {
// D3DXBuffer and D3DBlob share an IID and layout, but own different host
// payloads. Resolve this IID on the object rather than replacing the global
// D3DBlob mapping used by the D3D11 compiler.
const uint8_t buffer_iid[] = {0x08, 0xfb, 0xa5, 0x8b, 0x95, 0x51, 0xe2, 0x40,
                              0xac, 0x58, 0x0d, 0x98, 0x9c, 0x3a, 0x01, 0x02};
void buffer_query(X86 *c) {
    ComObj *o = com_this_arg(c, IF_D3DXBUFFER);
    uint32_t iid = arg(c, 1), out = arg(c, 2);
    if (!o || !iid || !out || !gm_fits(iid, 16) || !gm_fits(out, 4)) {
        set_eax(c, E_POINTER);
        return;
    }
    if (std::memcmp(gm_ptr(iid), buffer_iid, 16) == 0) {
        wr32(out, com_view(o, IF_D3DXBUFFER));
        com_addref(o);
        set_eax(c, S_OK);
    } else
        com_QueryInterface(c);
}
void buffer_destroy(ComObj *o) {
    if (o->buffer_data)
        heap_free(o->buffer_data);
    o->buffer_data = o->buffer_bytes = 0;
}
void buffer_pointer(X86 *c) {
    ComObj *o = com_this_arg(c, IF_D3DXBUFFER);
    set_eax(c, o ? o->buffer_data : 0);
}
void buffer_size(X86 *c) {
    ComObj *o = com_this_arg(c, IF_D3DXBUFFER);
    set_eax(c, o ? o->buffer_bytes : 0);
}
// Every returned buffer owns guest memory independently of parser temporaries.
// The shader pointer stays valid through CreateVertexShader until Release.
uint32_t make_buffer(const void *data, uint32_t bytes) {
    ComObj *o = com_new(K_D3DXBUFFER);
    if (!o)
        return 0;
    o->buffer_data = heap_alloc(bytes, true, 4);
    o->buffer_bytes = bytes;
    uint32_t view = o->buffer_data ? com_view(o, IF_D3DXBUFFER) : 0;
    if (!view) {
        com_release(o);
        return 0;
    }
    std::memcpy(gm_ptr(o->buffer_data), data, bytes);
    return view;
}
void assemble_shader(X86 *c) {
    constexpr uint32_t invalid = 0x88760b59u; // D3DXERR_INVALIDDATA
    uint32_t src = arg(c, 0), bytes = arg(c, 1), out = arg(c, 5), errors = arg(c, 6);
    if (!out || !gm_fits(out, 4) || (errors && !gm_fits(errors, 4))) {
        set_eax(c, E_POINTER);
        return;
    }
    wr32(out, 0);
    if (errors)
        wr32(errors, 0);
    std::vector<uint32_t> words;
    std::string error;
    if (!src || !bytes || bytes > 1024 * 1024 || !gm_fits(src, bytes))
        error = "invalid shader source span\n";
    else if (arg(c, 2) || arg(c, 3))
        error = "shader macros and include callbacks are not supported\n";
    else {
        std::string_view source(reinterpret_cast<const char *>(gm_ptr(src)), bytes);
        if (!source.empty() && source.back() == '\0')
            source.remove_suffix(1);
        d9sh::assemble(source, words, error);
    }
    if (!error.empty()) {
        LOGW("d3dx9: assembly failed: %s", error.c_str());
        if (src && bytes <= 16384 && gm_fits(src, bytes))
            LOGW("d3dx9: rejected shader source:\n%.*s", int(bytes),
                 reinterpret_cast<const char *>(gm_ptr(src)));
        if (errors) {
            uint32_t buffer = make_buffer(error.c_str(), uint32_t(error.size() + 1));
            wr32(errors, buffer);
            if (!buffer) {
                set_eax(c, E_OUTOFMEMORY);
                return;
            }
        }
        set_eax(c, invalid);
        return;
    }
    uint32_t buffer = make_buffer(words.data(), uint32_t(words.size() * 4));
    if (!buffer) {
        set_eax(c, E_OUTOFMEMORY);
        return;
    }
    wr32(out, buffer);
    LOGV("d3dx9: assembled %u source bytes into %zu shader DWORDs", bytes, words.size());
    set_eax(c, S_OK);
}
} // namespace
void d3dx9_assembler_register() {
    static const ComMethod methods[] = {{"QueryInterface", 3, buffer_query},
                                        {"AddRef", 1, com_AddRef},
                                        {"Release", 1, com_Release},
                                        {"GetBufferPointer", 1, buffer_pointer},
                                        {"GetBufferSize", 1, buffer_size}};
    com_define(IF_D3DXBUFFER, "d3dx9_30.dll", "ID3DXBuffer", methods, std::size(methods));
    com_bind(IF_D3DXBUFFER, K_D3DXBUFFER);
    com_set_destructor(K_D3DXBUFFER, buffer_destroy);
    static const ImportShim exports[] = {
        {"d3dx9_26.dll", "D3DXAssembleShader", 7, assemble_shader},
        {"d3dx9_30.dll", "D3DXAssembleShader", 7, assemble_shader}};
    imports_register(exports, std::size(exports));
}
