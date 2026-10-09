// dls_synth.h - the DirectMusic synthesizer: DLS instruments a game
// downloads itself, played note by note on host audio channels.
//
// dmusic.cpp hands over each download (IDirectMusicPortDownload::Download)
// and each performance message (IDirectMusicPerformance::SendPMsg). The
// instruments and waves stay in the guest memory the game filled; a note-on
// finds the region for its key and velocity, and plays the region's wave on a
// host channel at the pitch, volume and pan the channel's controllers say.
#pragma once
#include <stdint.h>

namespace dls {

// A DMUS_DOWNLOADINFO buffer at guest `mem`, `size` bytes, downloaded under
// `id`. Waves (type 2) and instruments (types 1 and 3) are kept; anything
// else is ignored. Returns true when the buffer was understood.
bool download(uint32_t id, uint32_t mem, uint32_t size);
// The download is gone; notes playing its wave are stopped.
void unload(uint32_t id);
// A DMUS_PMSG at guest `msg`: MIDI (type 0), note (type 1) and patch (type 7)
// messages are played, the rest ignored.
void pmsg(uint32_t msg);
void reset();

// For tests and logs.
uint32_t notes_started();
uint32_t instruments();
uint32_t waves();

} // namespace dls
