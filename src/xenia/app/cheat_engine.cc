/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/cheat_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/threading.h"

#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif

namespace xe {
namespace app {

namespace {

constexpr uint32_t kPhysicalSize = 0x20000000;  // 512 MiB of console RAM.
constexpr size_t kMaxCandidates = 2000000;
constexpr double kFloatEpsilon = 1e-3;

double LoadValue(const uint8_t* p, CheatEngine::ValueType type) {
  switch (type) {
    case CheatEngine::ValueType::kUInt8:
      return double(p[0]);
    case CheatEngine::ValueType::kUInt16:
      return double((uint32_t(p[0]) << 8) | p[1]);
    case CheatEngine::ValueType::kUInt32:
      return double((uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                    (uint32_t(p[2]) << 8) | p[3]);
    case CheatEngine::ValueType::kFloat: {
      uint32_t bits = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                      (uint32_t(p[2]) << 8) | p[3];
      float f;
      std::memcpy(&f, &bits, sizeof(f));
      return double(f);
    }
  }
  return 0.0;
}

void StoreValue(uint8_t* p, CheatEngine::ValueType type, double value) {
  switch (type) {
    case CheatEngine::ValueType::kUInt8:
      p[0] = uint8_t(int64_t(value));
      break;
    case CheatEngine::ValueType::kUInt16: {
      uint16_t v = uint16_t(int64_t(value));
      p[0] = uint8_t(v >> 8);
      p[1] = uint8_t(v);
      break;
    }
    case CheatEngine::ValueType::kUInt32: {
      uint32_t v = uint32_t(int64_t(value));
      p[0] = uint8_t(v >> 24);
      p[1] = uint8_t(v >> 16);
      p[2] = uint8_t(v >> 8);
      p[3] = uint8_t(v);
      break;
    }
    case CheatEngine::ValueType::kFloat: {
      float f = float(value);
      uint32_t v;
      std::memcpy(&v, &f, sizeof(v));
      p[0] = uint8_t(v >> 24);
      p[1] = uint8_t(v >> 16);
      p[2] = uint8_t(v >> 8);
      p[3] = uint8_t(v);
      break;
    }
  }
}

bool ValuesEqual(double a, double b, CheatEngine::ValueType type) {
  if (type == CheatEngine::ValueType::kFloat) {
    return std::fabs(a - b) < kFloatEpsilon;
  }
  return a == b;
}

}  // namespace

const char* CheatEngine::ValueTypeName(ValueType type) {
  switch (type) {
    case ValueType::kUInt8:
      return "8-bit";
    case ValueType::kUInt16:
      return "16-bit";
    case ValueType::kUInt32:
      return "32-bit";
    case ValueType::kFloat:
      return "float";
  }
  return "?";
}

size_t CheatEngine::ValueTypeSize(ValueType type) {
  switch (type) {
    case ValueType::kUInt8:
      return 1;
    case ValueType::kUInt16:
      return 2;
    default:
      return 4;
  }
}

CheatEngine::CheatEngine(Memory* memory, std::filesystem::path save_path)
    : memory_(memory), save_path_(std::move(save_path)) {
  Load();
  worker_thread_ = std::thread(&CheatEngine::WorkerMain, this);
}

CheatEngine::~CheatEngine() {
  worker_running_ = false;
  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }
  Save();
}

std::vector<CheatEngine::Region> CheatEngine::QueryCommittedRegions() const {
  std::vector<Region> regions;
#if XE_PLATFORM_WIN32
  const uint8_t* base = memory_->physical_membase();
  uint32_t offset = 0;
  while (offset < kPhysicalSize) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(base + offset, &info, sizeof(info))) {
      break;
    }
    size_t region_size =
        std::min(size_t(info.RegionSize), size_t(kPhysicalSize - offset));
    bool readable =
        info.State == MEM_COMMIT && !(info.Protect & PAGE_GUARD) &&
        (info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                         PAGE_EXECUTE_READWRITE)) != 0;
    if (readable) {
      if (!regions.empty() &&
          regions.back().start + regions.back().size == offset) {
        regions.back().size += uint32_t(region_size);
      } else {
        regions.push_back({offset, uint32_t(region_size)});
      }
    }
    offset += uint32_t(region_size);
  }
#else
  regions.push_back({0, kPhysicalSize});
#endif
  return regions;
}

void CheatEngine::TakeSnapshot() {
  if (snapshot_.size() != kPhysicalSize) {
    snapshot_.assign(kPhysicalSize, 0);
  }
  snapshot_regions_ = QueryCommittedRegions();
  const uint8_t* base = memory_->physical_membase();
  for (const Region& region : snapshot_regions_) {
    std::memcpy(snapshot_.data() + region.start, base + region.start,
                region.size);
  }
  snapshot_valid_ = true;
}

void CheatEngine::NewScan(ValueType type) {
  scan_type_ = type;
  candidates_.clear();
  candidates_empty_means_all_ = true;
  results_truncated_ = false;
  TakeSnapshot();
}

void CheatEngine::ScanExact(double value) {
  if (!snapshot_valid_) {
    NewScan(scan_type_);
  }
  const uint8_t* base = memory_->physical_membase();
  const uint32_t value_size = uint32_t(ValueTypeSize(scan_type_));
  results_truncated_ = false;
  if (candidates_empty_means_all_) {
    std::vector<uint32_t> matches;
    for (const Region& region : QueryCommittedRegions()) {
      uint32_t addr = (region.start + value_size - 1) & ~(value_size - 1);
      uint32_t end = region.start + region.size;
      for (; addr + value_size <= end; addr += value_size) {
        if (ValuesEqual(LoadValue(base + addr, scan_type_), value,
                        scan_type_)) {
          if (matches.size() >= kMaxCandidates) {
            results_truncated_ = true;
            break;
          }
          matches.push_back(addr);
        }
      }
      if (results_truncated_) {
        break;
      }
    }
    candidates_ = std::move(matches);
    candidates_empty_means_all_ = false;
  } else {
    std::vector<uint32_t> kept;
    kept.reserve(candidates_.size());
    for (uint32_t addr : candidates_) {
      if (ValuesEqual(LoadValue(base + addr, scan_type_), value, scan_type_)) {
        kept.push_back(addr);
      }
    }
    candidates_ = std::move(kept);
  }
  TakeSnapshot();
}

void CheatEngine::ScanCompare(CompareOp op) {
  if (!snapshot_valid_) {
    return;
  }
  const uint8_t* base = memory_->physical_membase();
  const uint32_t value_size = uint32_t(ValueTypeSize(scan_type_));
  results_truncated_ = false;
  auto matches_op = [&](uint32_t addr) {
    double now = LoadValue(base + addr, scan_type_);
    double before = LoadValue(snapshot_.data() + addr, scan_type_);
    switch (op) {
      case CompareOp::kIncreased:
        return now > before;
      case CompareOp::kDecreased:
        return now < before;
      case CompareOp::kChanged:
        return !ValuesEqual(now, before, scan_type_);
      case CompareOp::kUnchanged:
        return ValuesEqual(now, before, scan_type_);
    }
    return false;
  };
  if (candidates_empty_means_all_) {
    std::vector<uint32_t> matches;
    for (const Region& region : QueryCommittedRegions()) {
      uint32_t addr = (region.start + value_size - 1) & ~(value_size - 1);
      uint32_t end = region.start + region.size;
      for (; addr + value_size <= end; addr += value_size) {
        if (matches_op(addr)) {
          if (matches.size() >= kMaxCandidates) {
            results_truncated_ = true;
            break;
          }
          matches.push_back(addr);
        }
      }
      if (results_truncated_) {
        break;
      }
    }
    candidates_ = std::move(matches);
    // If an "all memory" comparison produced too many hits, keep treating
    // everything as a candidate so the user can narrow it down further.
    if (results_truncated_) {
      candidates_.clear();
    } else {
      candidates_empty_means_all_ = false;
    }
  } else {
    std::vector<uint32_t> kept;
    kept.reserve(candidates_.size());
    for (uint32_t addr : candidates_) {
      if (matches_op(addr)) {
        kept.push_back(addr);
      }
    }
    candidates_ = std::move(kept);
  }
  TakeSnapshot();
}

bool CheatEngine::IsWritableAddress(uint32_t phys_addr, size_t size) const {
  if (uint64_t(phys_addr) + size > kPhysicalSize) {
    return false;
  }
#if XE_PLATFORM_WIN32
  MEMORY_BASIC_INFORMATION info;
  if (!VirtualQuery(memory_->physical_membase() + phys_addr, &info,
                    sizeof(info))) {
    return false;
  }
  return info.State == MEM_COMMIT && !(info.Protect & PAGE_GUARD) &&
         (info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) != 0;
#else
  return true;
#endif
}

double CheatEngine::ReadValue(uint32_t phys_addr, ValueType type) const {
  size_t size = ValueTypeSize(type);
  if (uint64_t(phys_addr) + size > kPhysicalSize) {
    return 0.0;
  }
#if XE_PLATFORM_WIN32
  MEMORY_BASIC_INFORMATION info;
  if (!VirtualQuery(memory_->physical_membase() + phys_addr, &info,
                    sizeof(info)) ||
      info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) ||
      (info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                       PAGE_EXECUTE_READWRITE)) == 0) {
    return 0.0;
  }
#endif
  return LoadValue(memory_->physical_membase() + phys_addr, type);
}

void CheatEngine::WriteValue(uint32_t phys_addr, ValueType type,
                             double value) {
  if (!IsWritableAddress(phys_addr, ValueTypeSize(type))) {
    return;
  }
  StoreValue(memory_->physical_membase() + phys_addr, type, value);
}

void CheatEngine::SetFrozen(uint32_t phys_addr, ValueType type, double value,
                            bool on) {
  std::lock_guard<std::mutex> lock(apply_mutex_);
  auto it = std::find_if(
      frozen_.begin(), frozen_.end(),
      [&](const CheatWrite& w) { return w.phys_addr == phys_addr; });
  if (on) {
    if (it != frozen_.end()) {
      it->type = type;
      it->value = value;
    } else {
      frozen_.push_back({phys_addr, type, value});
    }
  } else if (it != frozen_.end()) {
    frozen_.erase(it);
  }
}

bool CheatEngine::IsFrozen(uint32_t phys_addr) const {
  std::lock_guard<std::mutex> lock(apply_mutex_);
  return std::any_of(
      frozen_.begin(), frozen_.end(),
      [&](const CheatWrite& w) { return w.phys_addr == phys_addr; });
}

void CheatEngine::ClearFrozen() {
  std::lock_guard<std::mutex> lock(apply_mutex_);
  frozen_.clear();
}

size_t CheatEngine::frozen_count() const {
  std::lock_guard<std::mutex> lock(apply_mutex_);
  return frozen_.size();
}

void CheatEngine::AddCheat(const std::string& name, uint32_t phys_addr,
                           ValueType type, double value) {
  std::lock_guard<std::mutex> lock(apply_mutex_);
  Cheat cheat;
  cheat.name = name.empty() ? "Unnamed cheat" : name;
  cheat.enabled = true;
  cheat.writes.push_back({phys_addr, type, value});
  cheats_.push_back(std::move(cheat));
}

void CheatEngine::RemoveCheat(size_t index) {
  std::lock_guard<std::mutex> lock(apply_mutex_);
  if (index < cheats_.size()) {
    cheats_.erase(cheats_.begin() + index);
  }
}

void CheatEngine::SetCheatEnabled(size_t index, bool enabled) {
  std::lock_guard<std::mutex> lock(apply_mutex_);
  if (index < cheats_.size()) {
    cheats_[index].enabled = enabled;
  }
}

void CheatEngine::WorkerMain() {
  xe::threading::set_name("Cheat Engine");
  while (worker_running_) {
    {
      std::lock_guard<std::mutex> lock(apply_mutex_);
      for (const CheatWrite& write : frozen_) {
        if (IsWritableAddress(write.phys_addr, ValueTypeSize(write.type))) {
          StoreValue(memory_->physical_membase() + write.phys_addr, write.type,
                     write.value);
        }
      }
      for (const Cheat& cheat : cheats_) {
        if (!cheat.enabled) {
          continue;
        }
        for (const CheatWrite& write : cheat.writes) {
          if (IsWritableAddress(write.phys_addr, ValueTypeSize(write.type))) {
            StoreValue(memory_->physical_membase() + write.phys_addr,
                       write.type, write.value);
          }
        }
      }
    }
    xe::threading::Sleep(std::chrono::milliseconds(50));
  }
}

// Persistence format, one cheat per line:
//   name|enabled|addr:type:value[;addr:type:value...]
// addr is a hex guest physical address, type is the ValueType enum integer,
// value is a decimal number, e.g.:
//   Infinite Hearts|1|1A2B3C40:3:4.0
void CheatEngine::Save() {
  std::lock_guard<std::mutex> lock(apply_mutex_);
  std::ofstream file(save_path_, std::ofstream::trunc);
  if (!file.is_open()) {
    XELOGW("CheatEngine: failed to save cheats to {}", save_path_.string());
    return;
  }
  for (const Cheat& cheat : cheats_) {
    file << cheat.name << '|' << (cheat.enabled ? 1 : 0) << '|';
    for (size_t i = 0; i < cheat.writes.size(); ++i) {
      const CheatWrite& write = cheat.writes[i];
      if (i) {
        file << ';';
      }
      char addr_buffer[16];
      std::snprintf(addr_buffer, sizeof(addr_buffer), "%08X", write.phys_addr);
      file << addr_buffer << ':' << int(write.type) << ':' << write.value;
    }
    file << '\n';
  }
}

void CheatEngine::Load() {
  std::lock_guard<std::mutex> lock(apply_mutex_);
  cheats_.clear();
  std::ifstream file(save_path_);
  if (!file.is_open()) {
    return;
  }
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty()) {
      continue;
    }
    size_t first_sep = line.find('|');
    size_t second_sep = first_sep == std::string::npos
                            ? std::string::npos
                            : line.find('|', first_sep + 1);
    if (second_sep == std::string::npos) {
      continue;
    }
    Cheat cheat;
    cheat.name = line.substr(0, first_sep);
    cheat.enabled =
        line.substr(first_sep + 1, second_sep - first_sep - 1) == "1";
    std::stringstream writes_stream(line.substr(second_sep + 1));
    std::string write_text;
    while (std::getline(writes_stream, write_text, ';')) {
      unsigned int addr = 0;
      int type = 0;
      double value = 0.0;
      if (std::sscanf(write_text.c_str(), "%x:%d:%lf", &addr, &type,
                      &value) == 3 &&
          type >= 0 && type <= int(ValueType::kFloat)) {
        cheat.writes.push_back({uint32_t(addr), ValueType(type), value});
      }
    }
    if (!cheat.writes.empty()) {
      cheats_.push_back(std::move(cheat));
    }
  }
}

}  // namespace app
}  // namespace xe
