// mmio.cpp - WINMM's multimedia file I/O (mmioOpen, mmioRead, mmioDescend ...).
//
// Every open file is held whole in guest memory, which is what makes the
// buffered-access calls (mmioGetInfo, mmioAdvance, mmioSetInfo) exact: the
// MMIOINFO they hand out points at real guest bytes the caller reads in
// place. A file the guest names is read in at open; a memory file
// (fccIOProc 'MEM ', with pchBuffer and cchBuffer) is the caller's own
// buffer, as Windows does it. Gamebryo parses its WAV sounds this way, from
// buffers it has already read out of its archives. Writes extend a host-side
// copy that is flushed back to the file at close.
//
// MMIOINFO (72 bytes): dwFlags 0, fccIOProc 4, pIOProc 8, wErrorRet 12,
// htask 16, cchBuffer 20, pchBuffer 24, pchNext 28, pchEndRead 32,
// pchEndWrite 36, lBufOffset 40, lDiskOffset 44, adwInfo[3] 48,
// dwReserved1 60, dwReserved2 64, hmmio 68.
// MMCKINFO (20 bytes): ckid 0, cksize 4, fccType 8, dwDataOffset 12, dwFlags 16.
#include "imports.h"
#include "memory.h"
#include "win32.h"

#include <stdio.h>
#include <string.h>

#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace {

const uint32_t FOURCC_MEM = 0x204d454du;  // 'MEM '
const uint32_t FOURCC_RIFF = 0x46464952u; // 'RIFF'
const uint32_t FOURCC_LIST = 0x5453494cu; // 'LIST'
const uint32_t MMIO_WRITE = 0x1, MMIO_READWRITE = 0x2, MMIO_CREATE = 0x1000;
const uint32_t MMIO_FINDCHUNK = 0x10, MMIO_FINDRIFF = 0x20, MMIO_FINDLIST = 0x40;
const uint32_t MMIOERR_FILENOTFOUND = 257, MMIOERR_OUTOFMEMORY = 258, MMIOERR_CANNOTOPEN = 259;
const uint32_t MMIOERR_CANNOTSEEK = 262, MMIOERR_CHUNKNOTFOUND = 265, MMIOERR_INVALIDFILE = 267;

struct Mmio {
    uint32_t base = 0; // guest bytes of the whole file
    uint32_t size = 0; // bytes of it that hold data
    uint32_t cap = 0;  // bytes the guest block can hold
    uint32_t pos = 0;
    bool owned = false; // base was allocated here (a named file)
    bool writable = false;
    bool dirty = false;
    std::string path; // host path to flush to, for a named writable file
};

std::map<uint32_t, Mmio> &files() {
    static auto *m = new std::map<uint32_t, Mmio>();
    return *m;
}
uint32_t g_next = 0x00040004u;

Mmio *find(uint32_t h) {
    auto it = files().find(h);
    return it == files().end() ? nullptr : &it->second;
}

void fail_info(uint32_t info, uint32_t err) {
    if (info && gm_valid(info, 16))
        wr16(info + 12, (uint16_t)err);
}

// Room for `need` bytes from the start; a memory file cannot grow.
bool reserve(Mmio &f, uint32_t need) {
    if (need <= f.cap)
        return true;
    if (!f.owned)
        return false;
    uint32_t cap = f.cap ? f.cap : 4096;
    while (cap < need && cap < 0x40000000u)
        cap *= 2;
    uint32_t grown = heap_realloc(f.base, cap, true);
    if (!grown)
        return false;
    f.base = grown;
    f.cap = cap;
    return true;
}

// mmioOpenA(name, info, flags)
void mmio_open(X86 *c) {
    uint32_t name = arg(c, 0), info = arg(c, 1), flags = arg(c, 2);
    bool write = (flags & (MMIO_WRITE | MMIO_READWRITE | MMIO_CREATE)) != 0;
    Mmio f;
    f.writable = write;
    if (info && gm_valid(info, 72) && rd32(info + 4) == FOURCC_MEM) {
        f.base = rd32(info + 24);
        f.cap = f.size = rd32(info + 20);
        if (!f.base || !gm_valid(f.base, f.size)) {
            fail_info(info, MMIOERR_OUTOFMEMORY);
            set_eax(c, 0);
            return;
        }
    } else {
        std::string guest = name ? gm_str(name, 260) : std::string();
        std::string host = guest.empty() ? std::string() : win32_host_path(guest, write);
        if (host.empty()) {
            fail_info(info, MMIOERR_FILENOTFOUND);
            set_eax(c, 0);
            return;
        }
        std::vector<uint8_t> bytes;
        if (!(flags & MMIO_CREATE)) {
            FILE *fp = fopen(host.c_str(), "rb");
            if (!fp) {
                fail_info(info, MMIOERR_CANNOTOPEN);
                set_eax(c, 0);
                return;
            }
            uint8_t chunk[16384];
            size_t n;
            while ((n = fread(chunk, 1, sizeof chunk, fp)) > 0)
                bytes.insert(bytes.end(), chunk, chunk + n);
            fclose(fp);
        }
        f.owned = true;
        f.cap = bytes.size() > 4096 ? (uint32_t)bytes.size() : 4096;
        f.base = heap_alloc(f.cap, true);
        if (!f.base) {
            fail_info(info, MMIOERR_OUTOFMEMORY);
            set_eax(c, 0);
            return;
        }
        if (!bytes.empty())
            memcpy(g_mem + f.base, bytes.data(), bytes.size());
        f.size = (uint32_t)bytes.size();
        f.path = host;
        f.dirty = (flags & MMIO_CREATE) != 0;
    }
    uint32_t h = g_next;
    g_next += 4;
    files()[h] = f;
    if (info && gm_valid(info, 72)) {
        wr16(info + 12, 0);
        wr32(info + 68, h);
    }
    set_eax(c, h);
}

void flush(Mmio &f) {
    if (!f.dirty || f.path.empty())
        return;
    FILE *fp = fopen(f.path.c_str(), "wb");
    if (fp) {
        fwrite(g_mem + f.base, 1, f.size, fp);
        fclose(fp);
        win32_invalidate_dir_cache();
    }
    f.dirty = false;
}

// mmioClose(h, flags)
void mmio_close(X86 *c) {
    auto it = files().find(arg(c, 0));
    if (it == files().end()) {
        set_eax(c, 0);
        return;
    }
    flush(it->second);
    if (it->second.owned)
        heap_free(it->second.base);
    files().erase(it);
    set_eax(c, 0);
}

// mmioRead(h, buffer, count): bytes read, 0 at the end, -1 on error.
void mmio_read(X86 *c) {
    Mmio *f = find(arg(c, 0));
    uint32_t buf = arg(c, 1), n = arg(c, 2);
    if (!f || (int32_t)n < 0) {
        set_eax(c, 0xffffffffu);
        return;
    }
    uint32_t left = f->pos < f->size ? f->size - f->pos : 0;
    if (n > left)
        n = left;
    if (n && !gm_valid(buf, n)) {
        set_eax(c, 0xffffffffu);
        return;
    }
    memmove(g_mem + buf, g_mem + f->base + f->pos, n);
    f->pos += n;
    set_eax(c, n);
}

// mmioWrite(h, buffer, count)
void mmio_write(X86 *c) {
    Mmio *f = find(arg(c, 0));
    uint32_t buf = arg(c, 1), n = arg(c, 2);
    if (!f || !f->writable || (n && !gm_valid(buf, n)) || !reserve(*f, f->pos + n)) {
        set_eax(c, 0xffffffffu);
        return;
    }
    memmove(g_mem + f->base + f->pos, g_mem + buf, n);
    f->pos += n;
    if (f->pos > f->size)
        f->size = f->pos;
    f->dirty = true;
    set_eax(c, n);
}

// mmioSeek(h, offset, origin): the new position, or -1.
void mmio_seek(X86 *c) {
    Mmio *f = find(arg(c, 0));
    int32_t off = (int32_t)arg(c, 1);
    uint32_t origin = arg(c, 2);
    if (!f) {
        set_eax(c, 0xffffffffu);
        return;
    }
    int64_t from = origin == 1 ? f->pos : origin == 2 ? f->size : 0;
    int64_t to = from + off;
    if (to < 0 || to > 0x7fffffff) {
        set_eax(c, 0xffffffffu);
        return;
    }
    f->pos = (uint32_t)to;
    set_eax(c, f->pos);
}

uint32_t rd_file32(const Mmio &f, uint32_t at) {
    return at + 4 <= f.size ? rd32(f.base + at) : 0;
}

// mmioDescend(h, MMCKINFO *ck, const MMCKINFO *parent, flags)
void mmio_descend(X86 *c) {
    Mmio *f = find(arg(c, 0));
    uint32_t ck = arg(c, 1), parent = arg(c, 2), flags = arg(c, 3);
    if (!f || !ck || !gm_valid(ck, 20)) {
        set_eax(c, MMIOERR_CHUNKNOTFOUND);
        return;
    }
    uint32_t end = f->size;
    if (parent && gm_valid(parent, 20)) {
        uint64_t pend = (uint64_t)rd32(parent + 12) + rd32(parent + 4);
        if (pend < end)
            end = (uint32_t)pend;
    }
    uint32_t want_id = rd32(ck + 0), want_type = rd32(ck + 8);
    bool find_riff = (flags & MMIO_FINDRIFF) != 0, find_list = (flags & MMIO_FINDLIST) != 0;
    bool find_chunk = (flags & MMIO_FINDCHUNK) != 0;
    uint32_t at = f->pos;
    for (;;) {
        if (at + 8 > end) {
            set_eax(c, MMIOERR_CHUNKNOTFOUND);
            return;
        }
        uint32_t id = rd_file32(*f, at), size = rd_file32(*f, at + 4);
        bool form = id == FOURCC_RIFF || id == FOURCC_LIST;
        uint32_t type = form && at + 12 <= f->size ? rd_file32(*f, at + 8) : 0;
        bool match = true;
        if (find_riff)
            match = id == FOURCC_RIFF && type == want_type;
        else if (find_list)
            match = id == FOURCC_LIST && type == want_type;
        else if (find_chunk)
            match = id == want_id;
        if (match) {
            wr32(ck + 0, id);
            wr32(ck + 4, size);
            wr32(ck + 8, type);
            wr32(ck + 12, at + 8);
            wr32(ck + 16, 0);
            f->pos = at + (form ? 12 : 8);
            set_eax(c, 0);
            return;
        }
        uint64_t next = (uint64_t)at + 8 + size + (size & 1);
        if (next > end || next <= at) {
            set_eax(c, MMIOERR_CHUNKNOTFOUND);
            return;
        }
        at = (uint32_t)next;
    }
}

// mmioAscend(h, MMCKINFO *ck, flags): past the chunk and its pad byte. A
// chunk written since its descent (MMIO_DIRTY, dwFlags 2) gets its size.
void mmio_ascend(X86 *c) {
    Mmio *f = find(arg(c, 0));
    uint32_t ck = arg(c, 1);
    if (!f || !ck || !gm_valid(ck, 20)) {
        set_eax(c, MMIOERR_CANNOTSEEK);
        return;
    }
    uint32_t data = rd32(ck + 12), size = rd32(ck + 4);
    if ((rd32(ck + 16) & 2) && f->writable && data >= 4 && f->pos >= data) {
        size = f->pos - data;
        wr32(ck + 4, size);
        if (data - 4 + 4 <= f->size)
            wr32(f->base + data - 4, size);
        f->dirty = true;
    }
    uint64_t next = (uint64_t)data + size + (size & 1);
    f->pos = next > 0x7fffffffu ? 0x7fffffffu : (uint32_t)next;
    set_eax(c, 0);
}

// mmioCreateChunk(h, MMCKINFO *ck, flags): writes the header at the current
// position and descends into it.
void mmio_create_chunk(X86 *c) {
    Mmio *f = find(arg(c, 0));
    uint32_t ck = arg(c, 1), flags = arg(c, 2);
    if (!f || !f->writable || !ck || !gm_valid(ck, 20)) {
        set_eax(c, MMIOERR_CANNOTSEEK);
        return;
    }
    bool form = (flags & (MMIO_FINDRIFF | MMIO_FINDLIST)) != 0;
    uint32_t id = (flags & MMIO_FINDRIFF)   ? FOURCC_RIFF
                  : (flags & MMIO_FINDLIST) ? FOURCC_LIST
                                            : rd32(ck);
    uint32_t hdr = form ? 12 : 8;
    if (!reserve(*f, f->pos + hdr)) {
        set_eax(c, MMIOERR_OUTOFMEMORY);
        return;
    }
    wr32(f->base + f->pos, id);
    wr32(f->base + f->pos + 4, rd32(ck + 4));
    if (form)
        wr32(f->base + f->pos + 8, rd32(ck + 8));
    wr32(ck, id);
    wr32(ck + 12, f->pos + 8);
    wr32(ck + 16, 2); // MMIO_DIRTY
    f->pos += hdr;
    if (f->pos > f->size)
        f->size = f->pos;
    f->dirty = true;
    set_eax(c, 0);
}

// The buffer view of the file: the whole of it is the buffer.
void fill_info(const Mmio &f, uint32_t h, uint32_t info) {
    wr32(info + 20, f.cap);           // cchBuffer
    wr32(info + 24, f.base);          // pchBuffer
    wr32(info + 28, f.base + f.pos);  // pchNext
    wr32(info + 32, f.base + f.size); // pchEndRead
    wr32(info + 36, f.base + f.cap);  // pchEndWrite
    wr32(info + 40, 0);               // lBufOffset
    wr32(info + 44, f.size);          // lDiskOffset: everything is read in
    wr32(info + 68, h);
}

// mmioGetInfo(h, MMIOINFO *info, flags)
void mmio_get_info(X86 *c) {
    uint32_t h = arg(c, 0), info = arg(c, 1);
    Mmio *f = find(h);
    if (!f || !info || !gm_valid(info, 72)) {
        set_eax(c, MMIOERR_INVALIDFILE);
        return;
    }
    memset(g_mem + info, 0, 72);
    wr32(info + 4, f->owned ? 0x20534f44u : FOURCC_MEM); // 'DOS ' or 'MEM '
    fill_info(*f, h, info);
    set_eax(c, 0);
}

// Takes the caller's read/write position back from pchNext.
void take_position(Mmio &f, uint32_t info) {
    uint32_t next = rd32(info + 28);
    if (next >= f.base && next <= f.base + f.cap) {
        uint32_t pos = next - f.base;
        if (f.writable && pos > f.size && (rd32(info + 0) & 0x10000000u)) { // MMIO_DIRTY
            f.size = pos;
            f.dirty = true;
        }
        f.pos = pos;
    }
}

// mmioSetInfo(h, const MMIOINFO *info, flags)
void mmio_set_info(X86 *c) {
    Mmio *f = find(arg(c, 0));
    uint32_t info = arg(c, 1);
    if (f && info && gm_valid(info, 72))
        take_position(*f, info);
    set_eax(c, 0);
}

// mmioAdvance(h, MMIOINFO *info, flags): there is no more to read in, so
// the view is the same buffer from the caller's position. A writer whose
// buffer is full gets it grown when this is a named file.
void mmio_advance(X86 *c) {
    uint32_t h = arg(c, 0), info = arg(c, 1), flags = arg(c, 2);
    Mmio *f = find(h);
    if (!f || !info || !gm_valid(info, 72)) {
        set_eax(c, MMIOERR_INVALIDFILE);
        return;
    }
    take_position(*f, info);
    if ((flags & MMIO_WRITE) && f->pos >= f->cap && !reserve(*f, f->cap * 2)) {
        set_eax(c, 0x10e); // MMIOERR_CANNOTEXPAND
        return;
    }
    fill_info(*f, h, info);
    set_eax(c, 0);
}

void mmio_flush(X86 *c) {
    Mmio *f = find(arg(c, 0));
    if (f)
        flush(*f);
    set_eax(c, 0);
}

void mmio_set_buffer(X86 *c) {
    set_eax(c, 0);
}

// mmioStringToFOURCCA(text, flags): the first four characters, blank padded,
// upper-cased with MMIO_TOUPPER (0x10).
void mmio_string_to_fourcc(X86 *c) {
    std::string s = gm_str(arg(c, 0), 4);
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        uint8_t ch = i < (int)s.size() ? (uint8_t)s[i] : ' ';
        if ((arg(c, 1) & 0x10) && ch >= 'a' && ch <= 'z')
            ch = (uint8_t)(ch - 32);
        v |= (uint32_t)ch << (8 * i);
    }
    set_eax(c, v);
}

const ImportShim shims[] = {
    {"WINMM.dll", "mmioOpenA", 3, mmio_open},
    {"WINMM.dll", "mmioClose", 2, mmio_close},
    {"WINMM.dll", "mmioRead", 3, mmio_read},
    {"WINMM.dll", "mmioWrite", 3, mmio_write},
    {"WINMM.dll", "mmioSeek", 3, mmio_seek},
    {"WINMM.dll", "mmioDescend", 4, mmio_descend},
    {"WINMM.dll", "mmioAscend", 3, mmio_ascend},
    {"WINMM.dll", "mmioCreateChunk", 3, mmio_create_chunk},
    {"WINMM.dll", "mmioGetInfo", 3, mmio_get_info},
    {"WINMM.dll", "mmioSetInfo", 3, mmio_set_info},
    {"WINMM.dll", "mmioAdvance", 3, mmio_advance},
    {"WINMM.dll", "mmioFlush", 2, mmio_flush},
    {"WINMM.dll", "mmioSetBuffer", 4, mmio_set_buffer},
    {"WINMM.dll", "mmioStringToFOURCCA", 2, mmio_string_to_fourcc},
};

} // namespace

void mmio_register() {
    imports_register(shims, std::size(shims));
}
