/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_CHEAT_ENGINE_H_
#define XENIA_APP_CHEAT_ENGINE_H_

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "xenia/memory.h"

namespace xe {
namespace app {

// In-process memory scanner + value freezer for the cheat overlay.
// All addresses are guest physical addresses (the whole 512 MiB of console
// RAM is reachable through Memory::TranslatePhysical, regardless of which
// virtual mapping the game uses).
class CheatEngine {
 public:
  enum class ValueType : int {
    kUInt8 = 0,
    kUInt16 = 1,
    kUInt32 = 2,
    kFloat = 3,
  };

  enum class CompareOp : int {
    kIncreased = 0,
    kDecreased = 1,
    kChanged = 2,
    kUnchanged = 3,
  };

  struct CheatWrite {
    uint32_t phys_addr;
    ValueType type;
    double value;
  };

  struct Cheat {
    std::string name;
    bool enabled = false;
    std::vector<CheatWrite> writes;
  };

  static const char* ValueTypeName(ValueType type);
  static size_t ValueTypeSize(ValueType type);

  CheatEngine(Memory* memory, std::filesystem::path save_path);
  ~CheatEngine();

  // --- Scanning (UI thread only) ---
  // Drops all scan state and snapshots current memory. After this, compare
  // scans treat every address as a candidate ("unknown initial value").
  void NewScan(ValueType type);
  // Keeps only candidates currently equal to |value|. Usable as a first scan.
  void ScanExact(double value);
  // Keeps only candidates whose value changed vs the previous scan snapshot.
  // Requires NewScan first.
  void ScanCompare(CompareOp op);

  bool has_snapshot() const { return snapshot_valid_; }
  ValueType scan_type() const { return scan_type_; }
  size_t result_count() const { return candidates_.size(); }
  bool results_are_all_memory() const {
    return snapshot_valid_ && candidates_empty_means_all_;
  }
  bool results_truncated() const { return results_truncated_; }
  const std::vector<uint32_t>& results() const { return candidates_; }

  // --- Direct access ---
  double ReadValue(uint32_t phys_addr, ValueType type) const;
  void WriteValue(uint32_t phys_addr, ValueType type, double value);

  // --- Freezing (applied ~20x per second by a worker thread) ---
  void SetFrozen(uint32_t phys_addr, ValueType type, double value, bool on);
  bool IsFrozen(uint32_t phys_addr) const;
  void ClearFrozen();
  size_t frozen_count() const;

  // --- Named cheats (persisted to disk) ---
  std::vector<Cheat>& cheats() { return cheats_; }
  void AddCheat(const std::string& name, uint32_t phys_addr, ValueType type,
                double value);
  void RemoveCheat(size_t index);
  void SetCheatEnabled(size_t index, bool enabled);
  void Save();
  void Load();

 private:
  struct Region {
    uint32_t start;
    uint32_t size;
  };

  // Committed, readable regions of guest physical memory.
  std::vector<Region> QueryCommittedRegions() const;
  void TakeSnapshot();
  bool IsWritableAddress(uint32_t phys_addr, size_t size) const;
  void WorkerMain();

  Memory* memory_;
  std::filesystem::path save_path_;

  // Scan state.
  ValueType scan_type_ = ValueType::kUInt32;
  bool snapshot_valid_ = false;
  bool candidates_empty_means_all_ = true;
  bool results_truncated_ = false;
  std::vector<uint8_t> snapshot_;
  std::vector<Region> snapshot_regions_;
  std::vector<uint32_t> candidates_;

  // Freeze/cheat state, shared with the worker thread.
  mutable std::mutex apply_mutex_;
  std::vector<CheatWrite> frozen_;
  std::vector<Cheat> cheats_;

  std::atomic<bool> worker_running_{true};
  std::thread worker_thread_;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_CHEAT_ENGINE_H_
