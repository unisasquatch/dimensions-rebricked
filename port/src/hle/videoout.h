#pragma once
#include <cstdint>

namespace hle {

// Shows a registered display buffer and signals flip events (called by the Gnm submit path).
void VideoOutFlip(int buffer_index, int64_t flip_arg);
uint64_t VideoOutFlipCount();
// Shows display buffer `buffer_index` again without telling the game (debug frame freeze).
void VideoOutRepresent(int buffer_index);
// Debug remote: holds the game at its next flip and keeps re-rendering the last frame.
void SetFrameFreeze(bool on);
// Main thread, each loop pass: applies window changes asked for elsewhere (fullscreen from the menu).
void VideoOutMainThreadTick();
// Main thread only (SDL window rules).
void VideoOutSetTitle(const char* title);

}  // namespace hle
