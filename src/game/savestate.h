#pragma once
#include <cstdint>

namespace game {
namespace savestate {

// Slot count. Each slot is one file under saves/stateN.bin. 5 slots is a reasonable balance between
// disk space (up to ~500 MB each on a full RAM) and usefulness; the zero-chunk compression usually
// brings a real save down to 100-300 MB.
constexpr uint32_t kMaxSlots = 5;

// Request a save/load to/from `slot` (0..kMaxSlots-1). Called from the UI thread; the request is
// processed by the game thread on the next MenuTick, which is the only safe point to dump/restore RAM.
void RequestSave(uint32_t slot);
void RequestLoad(uint32_t slot);

// Delete the save in `slot`. Returns true if it existed and was deleted. Safe to call from any thread.
bool Delete(uint32_t slot);

// Whether `slot` has a save file, and its human-readable info ("level  2024-01-15 12:34" or "EMPTY").
bool HasSave(uint32_t slot);
const char* SaveInfo(uint32_t slot);

// Called from MenuTick each frame. Handles F5/F9 key edges and any pending requests.
void Tick();

}  // namespace savestate
}  // namespace game
