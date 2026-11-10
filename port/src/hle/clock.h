// The guest's clock. Normally real time; with LDPORT_FAST=<frame> it is frame-locked until that
// flip (exactly 1/60 s per flip, nothing drawn or presented) so test runs reach later scenes fast,
// then continues in real time from where the virtual clock left off.
#pragma once
#include <cstdint>

namespace hle {

// Nanoseconds since the process started, as the game sees them (monotonic).
uint64_t GuestNowNs();
// Offset (ns) to add to wall-clock time so calendar clocks stay consistent with GuestNowNs.
int64_t GuestWallOffsetNs();
// True while fast-forwarding: skip rendering and presentation, don't really sleep.
bool FastForwarding();
// Called once per flip.
void ClockOnFlip(uint64_t flips);
// Experiment: flips as fast as the game and GPU go, game clock in real time.
void SetUncapped(bool on);

}  // namespace hle
