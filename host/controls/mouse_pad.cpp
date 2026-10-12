// mouse_pad.cpp - see mouse_pad.h.
#include "mouse_pad.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace controls {

namespace {

bool pad_bit(const std::string &name, uint16_t *bit) {
    static const char *names[] = {"cross", "circle", "square", "triangle", "l1", "r1", "l2",
                                  "r2",    "l3",     "r3",     "select",   "start", "ps"};
    for (int i = 0; i < 13; ++i)
        if (name == names[i]) {
            *bit = uint16_t(1u << i);
            return true;
        }
    return false;
}

// Below this a deflection is let go: the stick returns to centre between
// strokes instead of creeping.
constexpr double kRest = 0.02;
// The smallest deflection a moving mouse produces, past the dead zone a
// game's own stick reading keeps (typically about 0.2).
constexpr double kMinimum = 0.28;

} // namespace

bool parse_mouse_pad(const std::string &spec, MousePadConfig *out) {
    MousePadConfig c;
    size_t at = 0;
    while (at <= spec.size()) {
        size_t end = spec.find(';', at);
        if (end == std::string::npos)
            end = spec.size();
        const std::string item = spec.substr(at, end - at);
        at = end + 1;
        if (item.empty())
            continue;
        const size_t eq = item.find('=');
        if (eq == std::string::npos)
            return false;
        const std::string k = item.substr(0, eq), v = item.substr(eq + 1);
        if (k == "stick") {
            if (v == "left")
                c.stick = 0;
            else if (v == "right")
                c.stick = 1;
            else if (v == "none" || v.empty())
                c.stick = -1;
            else
                return false;
        } else if (k == "hold") {
            if (!pad_bit(v, &c.hold_bit))
                return false;
        } else if (k == "fire") {
            if (!pad_bit(v, &c.fire_bit))
                return false;
        } else if (k == "speed") {
            char *e = nullptr;
            const double s = strtod(v.c_str(), &e);
            if (!e || *e || !(s > 0))
                return false;
            c.full_speed = s;
        } else {
            return false;
        }
    }
    *out = c;
    return true;
}

void MousePad::button(int button, bool down, bool captured) {
    if (!enabled())
        return;
    // A press counts only while the game owns the pointer; a release always
    // does, so nothing stays held.
    if (down && !captured)
        return;
    if (button == 0)
        fire_ = down;
    else if (button == 1)
        hold_ = down;
}

void MousePad::motion(double dx, double dy) {
    if (!enabled() || !hold_)
        return;
    acc_x_ += dx;
    acc_y_ += dy;
}

bool MousePad::tick(uint64_t now_ns, bool captured) {
    if (!enabled())
        return false;
    const PadState before = state_;
    if (!captured) {
        fire_ = hold_ = false;
        acc_x_ = acc_y_ = sx_ = sy_ = 0;
    }
    double dt = last_ns_ ? double(now_ns - last_ns_) / 1e9 : 0;
    last_ns_ = now_ns;
    dt = std::clamp(dt, 1.0 / 240, 0.1);
    auto target = [&](double acc) {
        if (acc == 0)
            return 0.0;
        double v = acc / dt / cfg_.full_speed;
        const double m = std::max(std::fabs(v), kMinimum);
        return std::copysign(std::min(m, 1.0), v);
    };
    if (hold_) {
        // Half the way to the new speed each frame: steady under the uneven
        // rate mouse events arrive at, and back to rest within a few frames.
        sx_ += (target(acc_x_) - sx_) * 0.5;
        sy_ += (target(acc_y_) - sy_) * 0.5;
        if (std::fabs(sx_) < kRest)
            sx_ = 0;
        if (std::fabs(sy_) < kRest)
            sy_ = 0;
    } else {
        sx_ = sy_ = 0;
    }
    acc_x_ = acc_y_ = 0;

    PadState s;
    if (fire_)
        s.buttons |= cfg_.fire_bit;
    if (hold_) {
        s.buttons |= cfg_.hold_bit;
        if (cfg_.hold_bit == kPadL2)
            s.l2 = 1;
        else if (cfg_.hold_bit == kPadR2)
            s.r2 = 1;
    }
    if (cfg_.fire_bit == kPadL2 && fire_)
        s.l2 = 1;
    else if (cfg_.fire_bit == kPadR2 && fire_)
        s.r2 = 1;
    (cfg_.stick == 0 ? s.lx : s.rx) = float(sx_);
    (cfg_.stick == 0 ? s.ly : s.ry) = float(sy_);
    state_ = s;
    return state_ != before;
}

MousePad &mouse_pad() {
    static MousePad pad;
    return pad;
}

} // namespace controls
