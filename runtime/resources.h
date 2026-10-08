// Shared reader for the loaded PE image's resource directory. Returned handles
// and payloads are guest addresses. No module or host pointer is substituted.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct ResourceName {
    bool is_string = false;
    uint32_t id = 0;
    std::string name; // UTF-8, decoded from the directory's length-prefixed UTF-16.
};

// Type and name accept integer IDs, UTF-16 "#123", or UTF-16 names. Select the
// first language and return its IMAGE_RESOURCE_DATA_ENTRY address, or zero.
//
// `module` selects the image: 0 or the EXE's base is the EXE, otherwise a
// resource-only module (below).
uint32_t resource_find(uint32_t type_id_or_name, uint32_t name_id_or_name,
                       std::string *why = nullptr, uint32_t module = 0);
// Validate a data-entry handle and return its image-backed payload and size.
uint32_t resource_data(uint32_t entry, uint32_t *size, std::string *why = nullptr,
                       uint32_t module = 0);
// Snapshot the names before guest callbacks run; no references into the directory
// or temporary guest buffers escape this reader. False explains failure in why.
bool resource_names(uint32_t type_id_or_name, std::vector<ResourceName> *names,
                    std::string *why = nullptr, uint32_t module = 0);

// Resource-only modules. A game that keeps its text in a DLL of string tables
// (a language DLL) LoadLibrary's the file and reads it with LoadString. Such a
// DLL has no shims and no translation: its headers and sections are copied
// into the guest heap, aligned like a module handle, and only its resources
// are ever read. Its code never runs, which is what LOAD_LIBRARY_AS_DATAFILE
// does and all a strings-only DLL needs.
//
// Maps the PE file at `host_path` and returns its guest base, or 0 when the
// file is not a 32-bit PE image. The same path maps once.
uint32_t resource_module_load(const std::string &host_path);
// True for 0, the EXE's base, and every base resource_module_load returned.
bool resource_module_known(uint32_t module);
// Forgets every mapping, for after mem_init discards the heap they lived in.
void resource_modules_reset();
