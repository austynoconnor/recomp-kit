// gamepad_sdl.cpp - see gamepad_sdl.h.
#include "gamepad_sdl.h"

#include "vpad.h"

#include <stdio.h>
#include <stdlib.h>

#include <vector>

namespace controls {

namespace {

// An axis or button counts only after it has been seen at rest: a device
// whose axis rests at full deflection (a speaker bar or headset Windows lists
// as a game controller, a wheel, a pad with a stuck trigger) otherwise holds
// a direction for ever, and a button held when the pad appears is not a press.
constexpr int kRestAxis = 8000;    // of 32767
constexpr int kActiveAxis = 16000; // movement that makes a pad the active one

struct Pad {
    SDL_Gamepad *pad = nullptr;
    SDL_JoystickID id = 0;
    bool axis_armed[kSdlAxisCount] = {};
    bool button_armed[kSdlPadButtonCount] = {};
};

std::vector<Pad> g_pads;     // every open pad
SDL_JoystickID g_active = 0; // the one the game reads: the one used last
bool g_buttons[kSdlPadButtonCount] = {};
int16_t g_axes[kSdlAxisCount] = {};

Pad *find(SDL_JoystickID id) {
    for (Pad &p : g_pads)
        if (p.id == id)
            return &p;
    return nullptr;
}

// The value the game may see: 0 / false until the input was seen at rest.
int16_t axis_of(Pad &p, int i, int16_t v) {
    if (!p.axis_armed[i]) {
        if (abs(v) >= kRestAxis)
            return 0;
        p.axis_armed[i] = true;
    }
    return v;
}
bool button_of(Pad &p, int i, bool down) {
    if (!p.button_armed[i]) {
        if (down)
            return false;
        p.button_armed[i] = true;
    }
    return down;
}

void publish() {
    vpad().set_source(kPadSourceController, pad_from_sdl(g_buttons, g_axes));
}

// Reads every mapped button and axis of the active pad from SDL's state.
void read_all() {
    Pad *p = find(g_active);
    for (int i = 0; i < kSdlPadButtonCount; ++i)
        g_buttons[i] = p && button_of(*p, i, SDL_GetGamepadButton(p->pad, SDL_GamepadButton(i)));
    for (int i = 0; i < kSdlAxisCount; ++i)
        g_axes[i] = p ? axis_of(*p, i, SDL_GetGamepadAxis(p->pad, SDL_GamepadAxis(i))) : 0;
}

void activate(SDL_JoystickID id) {
    if (id == g_active)
        return;
    g_active = id;
    if (Pad *p = find(id)) {
        const char *name = SDL_GetGamepadName(p->pad);
        fprintf(stderr, "[controls] using gamepad: %s\n", name ? name : "(unnamed)");
    }
    read_all();
    publish();
}

void open_pad(SDL_JoystickID id) {
    if (find(id))
        return;
    SDL_Gamepad *pad = SDL_OpenGamepad(id);
    if (!pad) {
        fprintf(stderr, "[controls] could not open gamepad %u: %s\n", unsigned(id), SDL_GetError());
        return;
    }
    Pad p;
    p.pad = pad;
    p.id = id;
    g_pads.push_back(p);
    const char *name = SDL_GetGamepadName(pad);
    fprintf(stderr, "[controls] gamepad connected: %s\n", name ? name : "(unnamed)");
    // Seen at rest now? Then its inputs count at once.
    Pad &added = g_pads.back();
    for (int i = 0; i < kSdlAxisCount; ++i)
        (void)axis_of(added, i, SDL_GetGamepadAxis(pad, SDL_GamepadAxis(i)));
    for (int i = 0; i < kSdlPadButtonCount; ++i)
        (void)button_of(added, i, SDL_GetGamepadButton(pad, SDL_GamepadButton(i)));
    if (!g_active)
        activate(id);
    vpad().set_controller_connected(true);
}

void open_all(SDL_JoystickID skip) {
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    if (!ids)
        return;
    for (int i = 0; i < count; ++i)
        if (ids[i] != skip)
            open_pad(ids[i]);
    SDL_free(ids);
}

void close_pad(SDL_JoystickID id) {
    for (size_t i = 0; i < g_pads.size(); ++i) {
        if (g_pads[i].id != id)
            continue;
        if (id == g_active) {
            // Zero first, so a mapped binding or a buffered reader sees every
            // release before the pad is reported gone.
            for (bool &b : g_buttons)
                b = false;
            for (int16_t &a : g_axes)
                a = 0;
            publish();
            g_active = 0;
        }
        SDL_CloseGamepad(g_pads[i].pad);
        g_pads.erase(g_pads.begin() + i);
        fprintf(stderr, "[controls] gamepad disconnected\n");
        break;
    }
    if (!g_active && !g_pads.empty())
        activate(g_pads.front().id);
    vpad().set_controller_connected(!g_pads.empty());
}

} // namespace

void gamepad_handle_event(const SDL_Event &e) {
    switch (e.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        open_pad(e.gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        close_pad(e.gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        Pad *p = find(e.gbutton.which);
        if (!p || e.gbutton.button >= kSdlPadButtonCount)
            break;
        const bool down = button_of(*p, e.gbutton.button, e.gbutton.down);
        if (down && p->id != g_active)
            activate(p->id); // the pad pressed last is the one in use
        if (p->id == g_active) {
            g_buttons[e.gbutton.button] = down;
            publish();
        }
        break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        Pad *p = find(e.gaxis.which);
        if (!p || e.gaxis.axis >= kSdlAxisCount)
            break;
        const int16_t v = axis_of(*p, e.gaxis.axis, e.gaxis.value);
        if (abs(v) >= kActiveAxis && p->id != g_active)
            activate(p->id);
        if (p->id == g_active) {
            g_axes[e.gaxis.axis] = v;
            publish();
        }
        break;
    }
    default:
        break;
    }
}

void gamepad_poll() {
    // A pad whose SDL_EVENT_GAMEPAD_ADDED went somewhere else (the launcher
    // drains its own events) never reaches gamepad_handle_event, so look for
    // new ones every kScanIntervalMs.
    constexpr uint64_t kScanIntervalMs = 2000;
    static uint64_t next_scan_ms = 0;
    const uint64_t now_ms = SDL_GetTicks();
    if (now_ms >= next_scan_ms) {
        next_scan_ms = now_ms + kScanIntervalMs;
        open_all(0);
    }
    if (!g_active)
        return;
    read_all();
    publish();
}

bool gamepad_connected() {
    return g_active != 0;
}

bool gamepad_rumble(uint16_t low, uint16_t high, uint32_t ms) {
    Pad *p = find(g_active);
    return p && SDL_RumbleGamepad(p->pad, low, high, ms);
}

} // namespace controls
