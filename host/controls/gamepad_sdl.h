// gamepad_sdl.h - physical controllers, through SDL_Gamepad: every pad is
// opened on hot-plug, the one pressed or moved last is read into the shared
// virtual pad as kPadSourceController and rumbled on request, and each axis
// and button counts only once it has been seen at rest (a device resting at
// full deflection, or a button held at connect, is ignored). Called only from the SDL host's thread (main.cpp and
// controls_host.cpp). SDL's standard mapping names DualSense, Xbox and MFi
// buttons alike, so there are no per-controller tables here.
// Design: docs/superpowers/specs/2026-09-17-touch-controls-design.md, 7.2.
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>

namespace controls {

// SDL_EVENT_GAMEPAD_ADDED opens the pad; SDL_EVENT_GAMEPAD_REMOVED of the
// active one zeroes its source (the game sees every release) and falls back
// to another open pad. Button and axis events of the active pad update the
// source at once, so a press and release inside one pump both reach the edge
// queue; a press (or a large axis move) on another pad makes it active.
void gamepad_handle_event(const SDL_Event &e);
// Once per pump, after the events: re-reads the active pad's whole state into
// the source (a no-op when nothing changed), and looks for new pads.
void gamepad_poll();
bool gamepad_connected();
// SDL_RumbleGamepad on the active pad; false when there is none or SDL refused.
bool gamepad_rumble(uint16_t low, uint16_t high, uint32_t ms);

} // namespace controls
