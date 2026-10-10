// key_remap.h - [controls.native] keys: which key the game sees for each
// physical key, for a game whose own keyboard defaults the port changes
// without touching its files (MGS2 moves with WASD this way). SDL-free: keys
// are SDL_Scancode values, spelled as in layout.h's scancode_from_name.
#pragma once

#include <stdint.h>
#include <string>

namespace controls {

class KeyRemap {
  public:
    static const int kScancodes = 512;

    // "W=Up;A=Left": physical=guest pairs. Unlisted keys reach the game as
    // themselves. False, with nothing changed, if a name is unknown.
    bool parse(const std::string &spec, std::string *error = nullptr);
    bool empty() const { return count_ == 0; }
    int target(int scancode) const;

    // A physical key went down or up. Answers the guest key to deliver and
    // whether to deliver it at all: two physical keys can feed one guest key
    // (W and the Up arrow), and the guest key is released only when both are.
    // A held key's repeats are delivered as downs.
    bool press(int scancode, bool down, int *guest);
    void release_all() { reset_state(); }

  private:
    void reset_state();
    int16_t map_[kScancodes] = {};
    bool physical_[kScancodes] = {};
    uint8_t held_[kScancodes] = {};
    int count_ = 0;
};

} // namespace controls
