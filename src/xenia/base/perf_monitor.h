/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_PERF_MONITOR_H_
#define XENIA_BASE_PERF_MONITOR_H_

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace xe {

// One aggregated row of the session performance log.
struct PerfSample {
  double uptime_seconds = 0.0;
  double interval_seconds = 0.0;
  uint64_t frames_total = 0;
  uint64_t frames_in_interval = 0;
  double fps_avg = 0.0;
  double fps_min = 0.0;
  double fps_max = 0.0;
  // Average FPS of the slowest 1% of ticks in the interval - this is the
  // number that reflects "stability" (stutter) rather than throughput.
  double fps_1pct_low = 0.0;
  double frame_ms_avg = 0.0;
  double cpu_process_percent = 0.0;
  double cpu_system_percent = 0.0;
  double ram_working_set_mb = 0.0;
  double ram_private_mb = 0.0;
  // Negative when the value could not be queried on this system.
  double vram_used_mb = -1.0;
  double vram_budget_mb = -1.0;
};

// Samples guest frame rate and host CPU/RAM/VRAM usage on a background
// thread, keeps the readings for live display, and appends aggregated rows
// to a CSV next to the executable.
//
// The log is per session: the previous file is deleted when the monitor
// starts, so what is on disk always belongs to the run that produced it.
class PerfMonitor {
 public:
  static PerfMonitor& Get();

  PerfMonitor(const PerfMonitor&) = delete;
  PerfMonitor& operator=(const PerfMonitor&) = delete;

  // Starts the sampler thread and truncates the previous session log.
  // Safe to call repeatedly; only the first call does anything. Does nothing
  // while the monitor is disabled, which is the default - the GPU thread calls
  // this on every swap, so enabling it later picks up from the next frame.
  void Start();
  void Shutdown();

  // Turns sampling on or off at runtime and remembers the choice in the
  // perf_monitor cvar, so the Performance panel can drive it without a config
  // edit and a restart.
  void SetEnabled(bool enabled);
  static bool enabled();

  // Counted once per guest swap. Cheap enough to call from the GPU thread.
  void NotifyGuestFrame() {
    frames_total_.fetch_add(1, std::memory_order_relaxed);
  }

  bool running() const { return running_.load(std::memory_order_acquire); }

  // Live readings, refreshed once a second regardless of the log interval.
  PerfSample live() const;
  // Aggregated rows written so far this session.
  std::vector<PerfSample> samples() const;
  const std::filesystem::path& log_path() const { return log_path_; }

 private:
  PerfMonitor() = default;
  ~PerfMonitor();

  void ThreadMain();
  void WriteCsvHeader();
  void AppendCsvRow(const PerfSample& sample);

  // Serializes Start/Shutdown: the GPU thread calls Start() on every swap
  // while the UI thread may be tearing the sampler down.
  std::mutex lifecycle_mutex_;

  std::atomic<uint64_t> frames_total_{0};
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  std::thread thread_;

  mutable std::mutex data_mutex_;
  PerfSample live_;
  std::vector<PerfSample> samples_;

  std::filesystem::path log_path_;
};

}  // namespace xe

#endif  // XENIA_BASE_PERF_MONITOR_H_
