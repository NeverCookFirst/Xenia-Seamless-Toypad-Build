/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_MOD_OVERLAY_H_
#define XENIA_VFS_MOD_OVERLAY_H_

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xe {
namespace vfs {

// Virtual file mods: same-length byte-range overlays applied to reads of
// files inside XContent (STFS) packages. The user's packages are never
// modified on disk - patches are applied on the fly in ReadSync.
//
// Mods live in <exe folder>\mods\<ModFolder>\mod.txt:
//   name = Super Sonic Infinite
//   desc = Sonic's Super form never runs out
//   patch = DLC16.DAT2 | 0xF493C8F | original\a.bin | modded\a.bin
// The original file is used to verify the user's package version before the
// overlay is trusted; on mismatch the mod is marked incompatible and skipped.
// Enabled state persists in mods\state.txt as "<ModFolder>=0|1" lines.
class ModOverlayRegistry {
 public:
  struct Patch {
    std::string target_file_lower;  // contained file name, e.g. "dlc16.dat2"
    uint64_t offset;                // offset within the contained file
    std::vector<uint8_t> original;
    std::vector<uint8_t> replacement;
  };

  struct Mod {
    std::string folder;
    std::string name;
    std::string description;
    std::atomic<bool> enabled{false};
    std::atomic<bool> incompatible{false};
    std::vector<Patch> patches;
  };

  static ModOverlayRegistry& Get();

  // Applies enabled overlays overlapping [file_offset, file_offset+size) of
  // the contained file |file_name| onto |buffer| (already filled with the
  // underlying package bytes). Cheap no-op when no mod targets the file.
  void Apply(const std::string& file_name, uint64_t file_offset,
             std::span<uint8_t> buffer);

  std::vector<Mod*> mods();
  void SetEnabled(Mod* mod, bool enabled);

 private:
  ModOverlayRegistry();
  void LoadMods();
  void LoadState();
  void SaveState();

  std::filesystem::path mods_dir_;
  std::vector<std::unique_ptr<Mod>> mods_;
  // contained-file-name (lower) -> patches touching it. Immutable after load.
  std::unordered_map<std::string, std::vector<std::pair<Mod*, const Patch*>>>
      by_file_;
  std::mutex state_mutex_;
};

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_MOD_OVERLAY_H_
