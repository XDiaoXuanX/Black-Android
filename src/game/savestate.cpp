// Save state / load state: dump the whole guest RAM (512 MB on the Xbox 360) to a file and restore it.
//
// The recompilation does not have a "PC to jump to": recompiled code is native, and the save/load only
// works because both happen at the same well-defined point in the game's main loop (MenuTick, once per
// frame). RAM is the game's state; restoring RAM at a later frame makes the game continue as if it were
// the earlier frame. Registers are NOT saved/restored: the game re-derives them from RAM each frame.
//
// Each save is a header (120 bytes) + the RAM in 64 KB chunks. A chunk that is entirely zeros is stored
// as a single byte (type 0) instead of 65537 bytes; any other chunk is stored as type 1 + 64 KB of raw
// data. Real CoD2 saves compress to roughly 100-300 MB (much of the 512 MB is unmapped or zero).
//
// F5 saves to the currently-selected slot, F9 loads it. Both are also available as panel buttons.
#include "game/savestate.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include <SDL3/SDL.h>

#include "common/options.h"
#include "core/log.h"
#include "game/layout.h"
#include "kernel/memory.h"

namespace fs = std::filesystem;
using namespace kernel;
using namespace layout;

namespace game {
namespace savestate {

namespace {

constexpr uint32_t kMagic = 0x53415645;  // "SAVE" little endian
constexpr uint32_t kVersion = 1;
constexpr size_t kRamSize = 512u * 1024u * 1024u;  // Xbox 360 physical RAM
constexpr size_t kChunkSize = 65536;
constexpr size_t kChunkCount = kRamSize / kChunkSize;

#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    uint32_t version;
    uint64_t timestamp;        // unix seconds
    char level_name[64];       // "menu" or "level" (the game's level name is not directly readable)
    uint32_t ram_size;         // kRamSize
    uint32_t reserved[9];      // future use; keeps the header at 120 bytes for alignment
};
#pragma pack(pop)
static_assert(sizeof(Header) == 120, "Header must be 120 bytes");

std::mutex g_lock;
int g_save_request = -1;
int g_load_request = -1;

// F5/F9 edge detection. SDL_GetKeyboardState returns the current held state; we track the previous
// frame's state to detect a fresh press.
bool g_save_held = false;
bool g_load_held = false;

// Info cache for the panel (slow to read the header each frame)
std::string g_info_cache[kMaxSlots];
bool g_info_loaded[kMaxSlots] = {};

std::string SavesDir() {
    return (fs::current_path() / "saves").string();
}

std::string SlotPath(uint32_t slot) {
    return (fs::path(SavesDir()) / ("state" + std::to_string(slot) + ".bin")).string();
}

void InvalidateInfo() {
    for (uint32_t i = 0; i < kMaxSlots; i++) {
        g_info_cache[i].clear();
        g_info_loaded[i] = false;
    }
}

// Try to find a guest RAM base. HostPtr(0) is the standard mapping on the Xbox 360 (physical RAM
// starts at VA 0). If a future kernel exposes a proper accessor, use that instead.
uint8_t* GuestRam() { return static_cast<uint8_t*>(HostPtr(0)); }

}  // namespace

bool HasSave(uint32_t slot) {
    if (slot >= kMaxSlots) return false;
    std::error_code ec;
    return fs::is_regular_file(SlotPath(slot), ec);
}

const char* SaveInfo(uint32_t slot) {
    if (slot >= kMaxSlots) return "";
    if (g_info_loaded[slot]) return g_info_cache[slot].c_str();
    g_info_loaded[slot] = true;
    if (!HasSave(slot)) {
        g_info_cache[slot] = "EMPTY";
        return g_info_cache[slot].c_str();
    }
    std::ifstream file(SlotPath(slot), std::ios::binary);
    if (!file) {
        g_info_cache[slot] = "UNREADABLE";
        return g_info_cache[slot].c_str();
    }
    Header h{};
    if (!file.read(reinterpret_cast<char*>(&h), sizeof(h)) || h.magic != kMagic || h.version != kVersion) {
        g_info_cache[slot] = "BAD FILE";
        return g_info_cache[slot].c_str();
    }
    char timebuf[32] = "?";
    std::time_t t = static_cast<std::time_t>(h.timestamp);
    std::tm* tm = std::localtime(&t);
    if (tm) std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M", tm);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s  %s", h.level_name[0] ? h.level_name : "unknown", timebuf);
    g_info_cache[slot] = buf;
    return g_info_cache[slot].c_str();
}

bool Delete(uint32_t slot) {
    if (slot >= kMaxSlots) return false;
    std::error_code ec;
    bool ok = fs::remove(SlotPath(slot), ec);
    if (ok) InvalidateInfo();
    return ok;
}

static bool SaveImpl(uint32_t slot) {
    if (slot >= kMaxSlots) return false;
    uint8_t* ram = GuestRam();
    if (!ram) {
        LOGW("savestate: no guest RAM base (HostPtr(0) returned null)");
        return false;
    }
    std::error_code ec;
    fs::create_directories(SavesDir(), ec);

    Header h{};
    h.magic = kMagic;
    h.version = kVersion;
    h.timestamp = static_cast<uint64_t>(std::time(nullptr));
    h.ram_size = static_cast<uint32_t>(kRamSize);
    std::strncpy(h.level_name, R32(kWorld) ? "level" : "menu", sizeof(h.level_name) - 1);

    // Atomic-ish: write to a .tmp, then rename. A crash during the write leaves the previous save intact.
    std::string final_path = SlotPath(slot);
    std::string tmp_path = final_path + ".tmp";
    std::ofstream file(tmp_path, std::ios::binary | std::ios::trunc);
    if (!file) {
        LOGW("savestate: cannot create %s", tmp_path.c_str());
        return false;
    }
    file.write(reinterpret_cast<const char*>(&h), sizeof(h));
    size_t written = sizeof(h);
    for (size_t i = 0; i < kChunkCount; i++) {
        const uint8_t* chunk = ram + i * kChunkSize;
        bool all_zero = true;
        for (size_t j = 0; j < kChunkSize; j++) {
            if (chunk[j]) { all_zero = false; break; }
        }
        uint8_t type = all_zero ? 0 : 1;
        file.write(reinterpret_cast<const char*>(&type), 1);
        written += 1;
        if (!all_zero) {
            file.write(reinterpret_cast<const char*>(chunk), kChunkSize);
            written += kChunkSize;
        }
        if (!file) {
            LOGW("savestate: write failed at chunk %zu (disk full?)", i);
            file.close();
            fs::remove(tmp_path, ec);
            return false;
        }
    }
    file.close();
    fs::rename(tmp_path, final_path, ec);
    if (ec) {
        LOGW("savestate: rename failed: %s", ec.message().c_str());
        return false;
    }
    InvalidateInfo();
    LOGI("savestate: saved slot %u (%.1f MB) to %s", slot, double(written) / (1024.0 * 1024.0), final_path.c_str());
    return true;
}

static bool LoadImpl(uint32_t slot) {
    if (slot >= kMaxSlots) return false;
    if (!HasSave(slot)) {
        LOGW("savestate: no save in slot %u", slot);
        return false;
    }
    std::ifstream file(SlotPath(slot), std::ios::binary);
    if (!file) return false;
    Header h{};
    if (!file.read(reinterpret_cast<char*>(&h), sizeof(h))) {
        LOGW("savestate: header read failed in slot %u", slot);
        return false;
    }
    if (h.magic != kMagic || h.version != kVersion) {
        LOGW("savestate: bad magic/version in slot %u (%08X v%u)", slot, h.magic, h.version);
        return false;
    }
    if (h.ram_size != kRamSize) {
        LOGW("savestate: RAM size mismatch in slot %u (file %u, expected %zu)", slot, h.ram_size, kRamSize);
        return false;
    }
    uint8_t* ram = GuestRam();
    if (!ram) {
        LOGW("savestate: no guest RAM base");
        return false;
    }
    for (size_t i = 0; i < kChunkCount; i++) {
        uint8_t type = 0;
        if (!file.read(reinterpret_cast<char*>(&type), 1)) {
            LOGW("savestate: truncated at chunk %zu", i);
            return false;
        }
        uint8_t* chunk = ram + i * kChunkSize;
        if (type == 0) {
            std::memset(chunk, 0, kChunkSize);
        } else if (type == 1) {
            if (!file.read(reinterpret_cast<char*>(chunk), kChunkSize)) {
                LOGW("savestate: raw chunk read failed at %zu", i);
                return false;
            }
        } else {
            LOGW("savestate: bad chunk type %u at %zu", type, i);
            return false;
        }
    }
    LOGI("savestate: loaded slot %u from %s", slot, SlotPath(slot).c_str());
    return true;
}

void RequestSave(uint32_t slot) {
    if (slot >= kMaxSlots) return;
    std::lock_guard<std::mutex> lock(g_lock);
    g_save_request = int(slot);
}

void RequestLoad(uint32_t slot) {
    if (slot >= kMaxSlots) return;
    std::lock_guard<std::mutex> lock(g_lock);
    g_load_request = int(slot);
}

void Tick() {
    // F5 / F9 edges via SDL. Works whenever the app window has focus, whether the panel is open or not.
    // The panel's own Save/Load buttons go through the request queue below, so they do not race the
    // keyboard path.
    const bool* keys = SDL_GetKeyboardState(nullptr);
    bool save_down = keys && keys[SDL_SCANCODE_F5];
    bool load_down = keys && keys[SDL_SCANCODE_F9];
    uint32_t slot = game::Opt().save_slot % kMaxSlots;
    if (save_down && !g_save_held) RequestSave(slot);
    if (load_down && !g_load_held) RequestLoad(slot);
    g_save_held = save_down;
    g_load_held = load_down;

    // Process pending requests (either from the keyboard above or from the panel buttons).
    int save_slot = -1, load_slot = -1;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        save_slot = g_save_request;
        load_slot = g_load_request;
        g_save_request = -1;
        g_load_request = -1;
    }
    if (save_slot >= 0) SaveImpl(uint32_t(save_slot));
    if (load_slot >= 0) LoadImpl(uint32_t(load_slot));
}

}  // namespace savestate
}  // namespace game
