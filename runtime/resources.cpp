#include "resources.h"
#include "loader.h"
#include "memory.h"
#include "../platform/os.h"

#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <map>

namespace {
// Resource-only modules: mapped size by guest base, and base by host path.
std::map<uint32_t, uint32_t> &resource_modules() {
    static auto *m = new std::map<uint32_t, uint32_t>();
    return *m;
}
std::map<std::string, uint32_t> &resource_module_paths() {
    static auto *m = new std::map<std::string, uint32_t>();
    return *m;
}
bool fail(std::string *why, const char *message) {
    if (why)
        *why = message;
    return false;
}

// Every directory-relative offset, count and payload RVA is checked before a
// guest read. PE resource strings carry a WORD length, not a NUL terminator.
struct Directory {
    uint32_t image = 0, image_size = 0, root = 0, size = 0;
    std::string *why;
    explicit Directory(std::string *reason) : why(reason) {
        if (why)
            why->clear();
    }
    bool image_range(uint32_t rva, uint32_t bytes) const {
        return rva <= image_size && bytes <= image_size - rva;
    }
    bool relative(uint32_t offset, uint32_t bytes) const {
        return offset <= size && bytes <= size - offset;
    }
    bool open(uint32_t module = 0) {
        image = loader_image_base();
        image_size = loader_image_size();
        if (module && module != image) {
            auto it = resource_modules().find(module);
            if (it == resource_modules().end())
                return fail(why, "not a module with resources");
            image = it->first;
            image_size = it->second;
        }
        if (!image || !gm_valid(image, image_size) || image_size < 64 || rd16(image) != 0x5a4d)
            return fail(why, "no loaded PE image");
        uint32_t nt = rd32(image + 0x3c);
        if (!image_range(nt, 24) || rd32(image + nt) != 0x4550)
            return fail(why, "invalid PE header");
        uint32_t opt = nt + 24, opt_size = rd16(image + nt + 20);
        if (opt_size < 120 || !image_range(opt, opt_size) || rd16(image + opt) != 0x10b ||
            rd32(image + opt + 92) < 3)
            return fail(why, "no PE32 resource data directory");
        uint32_t rva = rd32(image + opt + 112);
        size = rd32(image + opt + 116);
        if (!rva || size < 16 || !image_range(rva, size))
            return fail(why, "resource directory is missing or outside the image");
        root = image + rva;
        return true;
    }
    bool entries(uint32_t offset, uint32_t &first, uint32_t &count) {
        if (!relative(offset, 16))
            return fail(why, "truncated resource directory header");
        uint32_t p = root + offset;
        count = (uint32_t)rd16(p + 12) + rd16(p + 14);
        if (!relative(offset + 16, count * 8))
            return fail(why, "truncated resource directory entries");
        first = p + 16;
        return true;
    }
    bool name(uint32_t encoded, ResourceName &out) {
        out = {};
        if (!(encoded & 0x80000000u)) {
            if (encoded > 0xffff)
                return fail(why, "invalid integer resource identifier");
            out.id = encoded;
            return true;
        }
        uint32_t off = encoded & 0x7fffffffu;
        if (!relative(off, 2))
            return fail(why, "truncated resource name length");
        uint32_t units = rd16(root + off);
        if (!relative(off + 2, units * 2))
            return fail(why, "truncated resource name");
        out.is_string = true;
        out.name = gm_wstr(root + off + 2, units);
        return true;
    }
    bool child(uint32_t dir, const ResourceName &wanted, uint32_t &offset) {
        uint32_t first, count;
        if (!entries(dir, first, count))
            return false;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t p = first + i * 8;
            ResourceName candidate;
            if (!name(rd32(p), candidate))
                return false;
            if (candidate.is_string != wanted.is_string)
                continue;
            bool match = candidate.is_string
                             ? os_strcasecmp(candidate.name.c_str(), wanted.name.c_str()) == 0
                             : candidate.id == wanted.id;
            if (!match)
                continue;
            uint32_t next = rd32(p + 4);
            if (!(next & 0x80000000u))
                return fail(why, "expected a resource subdirectory");
            offset = next & 0x7fffffffu;
            if (!relative(offset, 16))
                return fail(why, "resource subdirectory outside its bounds");
            return true;
        }
        return fail(why, "resource identifier not found");
    }
    uint32_t data(uint32_t entry, uint32_t *bytes) {
        if (entry < root || !relative(entry - root, 16)) {
            fail(why, "invalid resource data entry");
            return 0;
        }
        uint32_t rva = rd32(entry), n = rd32(entry + 4);
        if (!image_range(rva, n)) {
            fail(why, "resource payload outside the image");
            return 0;
        }
        if (bytes)
            *bytes = n;
        return image + rva;
    }
};

bool identifier(uint32_t p, ResourceName &out, std::string *why) {
    out = {};
    if (p <= 0xffff) {
        out.id = p;
        return true;
    }
    if (!gm_valid(p, 2))
        return fail(why, "invalid resource identifier address");
    std::string text = gm_wstr(p);
    if (!text.empty() && text[0] == '#') {
        if (text.size() == 1)
            return fail(why, "empty numeric resource identifier");
        uint32_t id = 0;
        for (size_t i = 1; i < text.size(); ++i) {
            if (text[i] < '0' || text[i] > '9' || id > 6553)
                return fail(why, "invalid numeric resource identifier");
            id = id * 10 + (uint32_t)(text[i] - '0');
            if (id > 0xffff)
                return fail(why, "resource identifier exceeds 16 bits");
        }
        out.id = id;
    } else {
        out.is_string = true;
        out.name = text;
    }
    return true;
}
} // namespace

uint32_t resource_find(uint32_t type_id_or_name, uint32_t name_id_or_name, std::string *why,
                       uint32_t module) {
    Directory dir(why);
    ResourceName type, name;
    if (!dir.open(module) || !identifier(type_id_or_name, type, why) ||
        !identifier(name_id_or_name, name, why))
        return 0;
    uint32_t type_dir, name_dir, first, count;
    if (!dir.child(0, type, type_dir) || !dir.child(type_dir, name, name_dir) ||
        !dir.entries(name_dir, first, count))
        return 0;
    if (!count) {
        fail(why, "resource has no language entry");
        return 0;
    }
    uint32_t data_offset = rd32(first + 4);
    if ((data_offset & 0x80000000u) || !dir.relative(data_offset, 16)) {
        fail(why, "invalid resource language data entry");
        return 0;
    }
    uint32_t entry = dir.root + data_offset;
    return dir.data(entry, nullptr) ? entry : 0;
}
uint32_t resource_data(uint32_t entry, uint32_t *size, std::string *why, uint32_t module) {
    if (size)
        *size = 0;
    Directory dir(why);
    return dir.open(module) ? dir.data(entry, size) : 0;
}
bool resource_names(uint32_t type_id_or_name, std::vector<ResourceName> *names, std::string *why,
                    uint32_t module) {
    if (!names)
        return fail(why, "missing resource-name output");
    names->clear();
    Directory dir(why);
    ResourceName type;
    uint32_t type_dir, first, count;
    if (!dir.open(module) || !identifier(type_id_or_name, type, why) ||
        !dir.child(0, type, type_dir) || !dir.entries(type_dir, first, count))
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        ResourceName name;
        if (!dir.name(rd32(first + i * 8), name)) {
            names->clear();
            return false;
        }
        names->push_back(name);
    }
    return true;
}

// The file's headers and each section's raw bytes at its RVA, in a zeroed
// block of SizeOfImage. Relocations are not applied and imports are not
// bound: nothing in the block is ever executed.
uint32_t resource_module_load(const std::string &host_path) {
    auto known = resource_module_paths().find(host_path);
    if (known != resource_module_paths().end())
        return known->second;
    FILE *f = fopen(host_path.c_str(), "rb");
    if (!f)
        return 0;
    std::vector<uint8_t> file;
    uint8_t chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0 && file.size() < (64u << 20))
        file.insert(file.end(), chunk, chunk + n);
    fclose(f);
    auto u16 = [&](size_t at) -> uint32_t {
        return at + 2 <= file.size() ? (uint32_t)(file[at] | file[at + 1] << 8) : 0;
    };
    auto u32 = [&](size_t at) -> uint32_t {
        return at + 4 <= file.size()
                   ? (uint32_t)file[at] | (uint32_t)file[at + 1] << 8 |
                         (uint32_t)file[at + 2] << 16 | (uint32_t)file[at + 3] << 24
                   : 0;
    };
    if (u16(0) != 0x5a4d)
        return 0;
    uint32_t nt = u32(0x3c);
    if (u32(nt) != 0x4550 || u16(nt + 24) != 0x10b)
        return 0;
    uint32_t sections = u16(nt + 6), opt_size = u16(nt + 20);
    uint32_t image_size = u32(nt + 24 + 56), headers = u32(nt + 24 + 60);
    if (!image_size || image_size > (64u << 20) || headers > file.size() || headers > image_size)
        return 0;
    uint32_t base = heap_alloc(image_size, true, 0x10000);
    if (!base)
        return 0;
    memcpy(gm_ptr(base), file.data(), headers);
    uint32_t table = nt + 24 + opt_size;
    for (uint32_t i = 0; i < sections; ++i) {
        uint32_t sh = table + 40 * i;
        uint32_t vsize = u32(sh + 8), va = u32(sh + 12), raw_size = u32(sh + 16),
                 raw = u32(sh + 20);
        if (raw >= file.size() || va >= image_size)
            continue;
        uint32_t copy = std::min(raw_size, vsize ? vsize : raw_size);
        copy = std::min<uint32_t>(copy, (uint32_t)file.size() - raw);
        copy = std::min(copy, image_size - va);
        memcpy(gm_ptr(base + va), file.data() + raw, copy);
    }
    resource_modules()[base] = image_size;
    resource_module_paths()[host_path] = base;
    return base;
}
bool resource_module_known(uint32_t module) {
    return !module || module == loader_image_base() || resource_modules().count(module) != 0;
}
void resource_modules_reset() {
    resource_modules().clear();
    resource_module_paths().clear();
}
