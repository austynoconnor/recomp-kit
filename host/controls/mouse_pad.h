// mouse_pad.h - mouse aim for games that aim only with a controller stick.
//
// [controls.native] mouse_aim names a stick ("left" or "right"); the mouse
// then becomes one more source of the shared virtual pad (kPadSourceMouse):
// while the hold button (mouse_aim_hold, a pad button, default r2) is held by
// the right mouse button, mouse motion deflects that stick in proportion to
// its speed; the left mouse button presses mouse_fire (default cross). Keys
// still reach the game as keys. SDL-free, so the unit tests link it.
#pragma once

#include <cstdint>
#include <string>

#include "vpad.h"

namespace controls {

struct MousePadConfig {
    int stick = -1;            // 0 left, 1 right, -1 off
    uint16_t hold_bit = kPadR2; // pressed while the right mouse button is down
    uint16_t fire_bit = kPadCross;
    double full_speed = 1200;  // mouse counts per second for a full deflection
};

// "stick=left;hold=r2;fire=cross;speed=1200" (any key may be left out; an
// empty or "none" stick turns the feature off). False on a malformed spec,
// leaving *out unchanged.
bool parse_mouse_pad(const std::string &spec, MousePadConfig *out);

// The pure state machine; the host feeds it and publishes state().
class MousePad {
  public:
    void configure(const MousePadConfig &c) {
        cfg_ = c;
    }
    bool enabled() const {
        return cfg_.stick >= 0;
    }
    // button: 0 left, 1 right. `captured`: the game owns the pointer, so a
    // press is meant for it (a click that only takes the pointer back is not).
    void button(int button, bool down, bool captured);
    void motion(double dx, double dy);
    // Once per host frame. Turns the motion since the previous tick into a
    // stick deflection, and releases everything when the pointer is free.
    // True when state() changed.
    bool tick(uint64_t now_ns, bool captured);
    const PadState &state() const {
        return state_;
    }

  private:
    MousePadConfig cfg_;
    PadState state_;
    bool fire_ = false, hold_ = false;
    double acc_x_ = 0, acc_y_ = 0;
    double sx_ = 0, sy_ = 0;
    uint64_t last_ns_ = 0;
};

// The process-wide instance, published into vpad() as kPadSourceMouse.
MousePad &mouse_pad();

} // namespace controls
