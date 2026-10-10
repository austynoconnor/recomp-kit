// key_remap.cpp - see key_remap.h.
#include "key_remap.h"

#include "layout.h"

namespace controls {

bool KeyRemap::parse(const std::string &spec, std::string *error) {
    int16_t map[kScancodes] = {};
    int count = 0;
    size_t at = 0;
    while (at < spec.size()) {
        size_t end = spec.find(';', at);
        if (end == std::string::npos)
            end = spec.size();
        const std::string pair = spec.substr(at, end - at);
        at = end + 1;
        if (pair.empty())
            continue;
        const size_t eq = pair.find('=');
        const int from = eq == std::string::npos ? 0 : scancode_from_name(pair.substr(0, eq));
        const int to = eq == std::string::npos ? 0 : scancode_from_name(pair.substr(eq + 1));
        if (from <= 0 || to <= 0 || from >= kScancodes || to >= kScancodes) {
            if (error)
                *error = "unknown key in \"" + pair + "\"";
            return false;
        }
        map[from] = (int16_t)to;
        ++count;
    }
    for (int i = 0; i < kScancodes; ++i)
        map_[i] = map[i];
    count_ = count;
    reset_state();
    return true;
}

int KeyRemap::target(int scancode) const {
    if (scancode <= 0 || scancode >= kScancodes || !map_[scancode])
        return scancode;
    return map_[scancode];
}

bool KeyRemap::press(int scancode, bool down, int *guest) {
    const int to = target(scancode);
    *guest = to;
    if (scancode <= 0 || scancode >= kScancodes || to <= 0 || to >= kScancodes)
        return true;
    if (down) {
        if (physical_[scancode])
            return true; // a repeat of a key already counted
        physical_[scancode] = true;
        return ++held_[to] == 1;
    }
    if (!physical_[scancode])
        return false; // a release nobody pressed (focus came back mid-press)
    physical_[scancode] = false;
    return held_[to] > 0 && --held_[to] == 0;
}

void KeyRemap::reset_state() {
    for (int i = 0; i < kScancodes; ++i) {
        physical_[i] = false;
        held_[i] = 0;
    }
}

} // namespace controls
