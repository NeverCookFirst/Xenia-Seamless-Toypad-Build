/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/mod_overlay.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"

namespace xe {
namespace vfs {

namespace {

std::string ToLowerAscii(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return out;
}

std::string Trim(std::string_view s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string_view::npos) {
    return "";
  }
  size_t e = s.find_last_not_of(" \t\r\n");
  return std::string(s.substr(b, e - b + 1));
}

bool ReadWholeFile(const std::filesystem::path& path,
                   std::vector<uint8_t>& out) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    return false;
  }
  auto size = f.tellg();
  out.resize(size_t(size));
  f.seekg(0);
  f.read(reinterpret_cast<char*>(out.data()), size);
  return bool(f);
}

}  // namespace

ModOverlayRegistry& ModOverlayRegistry::Get() {
  static ModOverlayRegistry instance;
  return instance;
}

ModOverlayRegistry::ModOverlayRegistry() {
  mods_dir_ = xe::filesystem::GetExecutableFolder() / "mods";
  LoadMods();
  LoadState();
}

void ModOverlayRegistry::LoadMods() {
  std::error_code ec;
  if (!std::filesystem::is_directory(mods_dir_, ec)) {
    return;
  }
  for (auto& dir : std::filesystem::directory_iterator(mods_dir_, ec)) {
    if (!dir.is_directory()) {
      continue;
    }
    auto def_path = dir.path() / "mod.txt";
    std::ifstream def(def_path);
    if (!def) {
      continue;
    }
    auto mod = std::make_unique<Mod>();
    mod->folder = dir.path().filename().string();
    mod->name = mod->folder;
    bool valid = true;
    std::string line;
    while (std::getline(def, line)) {
      auto eq = line.find('=');
      if (line.empty() || line[0] == '#' || eq == std::string::npos) {
        continue;
      }
      std::string key = Trim(line.substr(0, eq));
      std::string value = Trim(line.substr(eq + 1));
      if (key == "name") {
        mod->name = value;
      } else if (key == "desc") {
        mod->description = value;
      } else if (key == "patch") {
        // <contained file> | <hex offset> | <original bin> | <modded bin>
        std::vector<std::string> parts;
        size_t pos = 0;
        while (true) {
          size_t bar = value.find('|', pos);
          parts.push_back(Trim(value.substr(
              pos, bar == std::string::npos ? std::string::npos : bar - pos)));
          if (bar == std::string::npos) {
            break;
          }
          pos = bar + 1;
        }
        if (parts.size() != 4) {
          XELOGE("ModOverlay: bad patch line in {}", def_path.string());
          valid = false;
          break;
        }
        Patch patch;
        patch.target_file_lower = ToLowerAscii(parts[0]);
        patch.offset = std::strtoull(parts[1].c_str(), nullptr, 0);
        if (!ReadWholeFile(dir.path() / parts[2], patch.original) ||
            !ReadWholeFile(dir.path() / parts[3], patch.replacement)) {
          XELOGE("ModOverlay: missing payload for {} in {}", parts[0],
                 def_path.string());
          valid = false;
          break;
        }
        if (patch.original.size() != patch.replacement.size() ||
            patch.original.empty()) {
          XELOGE("ModOverlay: payload size mismatch for {} in {}", parts[0],
                 def_path.string());
          valid = false;
          break;
        }
        mod->patches.push_back(std::move(patch));
      }
    }
    if (!valid || mod->patches.empty()) {
      continue;
    }
    for (auto& patch : mod->patches) {
      by_file_[patch.target_file_lower].emplace_back(mod.get(), &patch);
    }
    XELOGI("ModOverlay: loaded \"{}\" ({} patch(es))", mod->name,
           mod->patches.size());
    mods_.push_back(std::move(mod));
  }
}

void ModOverlayRegistry::LoadState() {
  std::ifstream state(mods_dir_ / "state.txt");
  if (!state) {
    return;
  }
  std::string line;
  while (std::getline(state, line)) {
    auto eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    std::string folder = Trim(line.substr(0, eq));
    bool enabled = Trim(line.substr(eq + 1)) == "1";
    for (auto& mod : mods_) {
      if (mod->folder == folder) {
        mod->enabled = enabled;
      }
    }
  }
}

void ModOverlayRegistry::SaveState() {
  std::ofstream state(mods_dir_ / "state.txt", std::ios::trunc);
  for (auto& mod : mods_) {
    state << mod->folder << "=" << (mod->enabled ? 1 : 0) << "\n";
  }
}

void ModOverlayRegistry::Apply(const std::string& file_name,
                               uint64_t file_offset,
                               std::span<uint8_t> buffer) {
  if (by_file_.empty() || buffer.empty()) {
    return;
  }
  auto it = by_file_.find(ToLowerAscii(file_name));
  if (it == by_file_.end()) {
    return;
  }
  uint64_t read_end = file_offset + buffer.size();
  for (auto& [mod, patch] : it->second) {
    if (!mod->enabled || mod->incompatible) {
      continue;
    }
    uint64_t patch_end = patch->offset + patch->original.size();
    if (patch->offset >= read_end || patch_end <= file_offset) {
      continue;
    }
    uint64_t begin = std::max(file_offset, patch->offset);
    uint64_t end = std::min(read_end, patch_end);
    size_t buf_pos = size_t(begin - file_offset);
    size_t patch_pos = size_t(begin - patch->offset);
    size_t length = size_t(end - begin);
    // Verify this slice against the expected original bytes so a different
    // TU/DLC version can never be silently corrupted by a stale mod.
    if (std::memcmp(buffer.data() + buf_pos, patch->original.data() + patch_pos,
                    length) != 0) {
      if (!mod->incompatible.exchange(true)) {
        XELOGE(
            "ModOverlay: \"{}\" does not match this package version "
            "(file {} @ 0x{:X}) - mod disabled",
            mod->name, file_name, patch->offset);
      }
      continue;
    }
    std::memcpy(buffer.data() + buf_pos, patch->replacement.data() + patch_pos,
                length);
  }
}

std::vector<ModOverlayRegistry::Mod*> ModOverlayRegistry::mods() {
  std::vector<Mod*> result;
  result.reserve(mods_.size());
  for (auto& mod : mods_) {
    result.push_back(mod.get());
  }
  return result;
}

void ModOverlayRegistry::SetEnabled(Mod* mod, bool enabled) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  mod->enabled = enabled;
  if (enabled) {
    mod->incompatible = false;  // allow re-verification
  }
  SaveState();
}

}  // namespace vfs
}  // namespace xe
