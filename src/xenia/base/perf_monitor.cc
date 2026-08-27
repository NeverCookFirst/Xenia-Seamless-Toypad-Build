/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/perf_monitor.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <numeric>
#include <system_error>

#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"

#if XE_PLATFORM_WIN32
// platform_win.h pulls in windows.h, which both of the headers below
// require, so it has to come first.
#include "xenia/base/platform_win.h"

#include <dxgi1_4.h>
#include <psapi.h>
#endif  // XE_PLATFORM_WIN32

DEFINE_bool(perf_monitor, true,
            "Sample guest frame rate and host CPU/RAM/VRAM usage during play "
            "and write them to perf_session.csv next to the executable. The "
            "previous session's file is deleted on startup.",
            "General");
DEFINE_bool(perf_log_to_file, false,
            "Also append the aggregated rows to perf_session.csv next to the "
            "executable. Off by default so that simply running the emulator "
            "never leaves files behind - the Performance panel works either "
            "way. The file belongs to a single session: the previous one is "
            "deleted when logging starts.",
            "General");
DEFINE_uint64(perf_monitor_interval, 60,
              "Seconds between aggregated rows in perf_session.csv. Live "
              "readings in the Performance panel refresh every second "
              "regardless of this value.",
              "General");

namespace xe {

namespace {

// Wall clock used for every interval measurement in this file.
using Clock = std::chrono::steady_clock;

double SecondsBetween(Clock::time_point from, Clock::time_point to) {
  return std::chrono::duration<double>(to - from).count();
}

#if XE_PLATFORM_WIN32

uint64_t FileTimeToUint(const FILETIME& ft) {
  ULARGE_INTEGER value;
  value.LowPart = ft.dwLowDateTime;
  value.HighPart = ft.dwHighDateTime;
  return value.QuadPart;
}

// Tracks process and system CPU time so each call reports the usage since
// the previous one.
class CpuUsageSampler {
 public:
  void Sample(double* process_percent, double* system_percent) {
    *process_percent = 0.0;
    *system_percent = 0.0;

    FILETIME creation, exit, kernel, user;
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel,
                        &user)) {
      uint64_t busy = FileTimeToUint(kernel) + FileTimeToUint(user);
      FILETIME now_ft;
      GetSystemTimeAsFileTime(&now_ft);
      uint64_t now = FileTimeToUint(now_ft);
      if (have_process_) {
        uint64_t wall_delta = now - last_process_wall_;
        uint64_t busy_delta = busy - last_process_busy_;
        if (wall_delta > 0 && processor_count_ > 0) {
          *process_percent = 100.0 * double(busy_delta) /
                             (double(wall_delta) * double(processor_count_));
        }
      }
      last_process_busy_ = busy;
      last_process_wall_ = now;
      have_process_ = true;
    }

    FILETIME idle_ft, sys_kernel_ft, sys_user_ft;
    if (GetSystemTimes(&idle_ft, &sys_kernel_ft, &sys_user_ft)) {
      uint64_t idle = FileTimeToUint(idle_ft);
      // Kernel time already includes idle time.
      uint64_t total =
          FileTimeToUint(sys_kernel_ft) + FileTimeToUint(sys_user_ft);
      if (have_system_) {
        uint64_t total_delta = total - last_system_total_;
        uint64_t idle_delta = idle - last_system_idle_;
        if (total_delta > 0 && total_delta >= idle_delta) {
          *system_percent =
              100.0 * double(total_delta - idle_delta) / double(total_delta);
        }
      }
      last_system_idle_ = idle;
      last_system_total_ = total;
      have_system_ = true;
    }

    *process_percent = std::clamp(*process_percent, 0.0, 100.0);
    *system_percent = std::clamp(*system_percent, 0.0, 100.0);
  }

 private:
  static DWORD QueryProcessorCount() {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwNumberOfProcessors ? info.dwNumberOfProcessors : 1;
  }

  DWORD processor_count_ = QueryProcessorCount();
  uint64_t last_process_busy_ = 0;
  uint64_t last_process_wall_ = 0;
  uint64_t last_system_idle_ = 0;
  uint64_t last_system_total_ = 0;
  bool have_process_ = false;
  bool have_system_ = false;
};

// PSAPI lives in kernel32 as K32GetProcessMemoryInfo on every Windows
// version we support, so resolving it dynamically keeps psapi.lib out of
// the link line.
using PFN_GetProcessMemoryInfo = BOOL(WINAPI*)(HANDLE, PPROCESS_MEMORY_COUNTERS,
                                               DWORD);

void SampleMemory(double* working_set_mb, double* private_mb) {
  *working_set_mb = 0.0;
  *private_mb = 0.0;
  static PFN_GetProcessMemoryInfo query = []() -> PFN_GetProcessMemoryInfo {
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!kernel32) {
      return nullptr;
    }
    return reinterpret_cast<PFN_GetProcessMemoryInfo>(
        GetProcAddress(kernel32, "K32GetProcessMemoryInfo"));
  }();
  if (!query) {
    return;
  }
  PROCESS_MEMORY_COUNTERS_EX counters = {};
  counters.cb = sizeof(counters);
  if (!query(GetCurrentProcess(),
             reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&counters),
             sizeof(counters))) {
    return;
  }
  const double kMb = 1024.0 * 1024.0;
  *working_set_mb = double(counters.WorkingSetSize) / kMb;
  *private_mb = double(counters.PrivateUsage) / kMb;
}

// Queries this process's video memory usage through DXGI. The DLL is loaded
// on demand so that base/ gains no link dependency on dxgi.lib.
class VideoMemorySampler {
 public:
  ~VideoMemorySampler() {
    if (adapter_) {
      adapter_->Release();
      adapter_ = nullptr;
    }
    // The DXGI module is intentionally left loaded; it is shared with the
    // graphics backend and unloading it here would be unsafe.
  }

  void Sample(double* used_mb, double* budget_mb) {
    *used_mb = -1.0;
    *budget_mb = -1.0;
    if (!EnsureAdapter()) {
      return;
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
    if (FAILED(adapter_->QueryVideoMemoryInfo(
            0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
      return;
    }
    const double kMb = 1024.0 * 1024.0;
    *used_mb = double(info.CurrentUsage) / kMb;
    *budget_mb = double(info.Budget) / kMb;
  }

 private:
  using PFN_CreateDXGIFactory1 = HRESULT(WINAPI*)(REFIID, void**);

  bool EnsureAdapter() {
    if (adapter_) {
      return true;
    }
    if (attempted_) {
      return false;
    }
    attempted_ = true;

    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (!dxgi) {
      return false;
    }
    auto create = reinterpret_cast<PFN_CreateDXGIFactory1>(
        GetProcAddress(dxgi, "CreateDXGIFactory1"));
    if (!create) {
      return false;
    }
    IDXGIFactory1* factory = nullptr;
    if (FAILED(create(__uuidof(IDXGIFactory1),
                      reinterpret_cast<void**>(&factory)))) {
      return false;
    }
    IDXGIAdapter1* adapter1 = nullptr;
    if (SUCCEEDED(factory->EnumAdapters1(0, &adapter1))) {
      adapter1->QueryInterface(__uuidof(IDXGIAdapter3),
                               reinterpret_cast<void**>(&adapter_));
      adapter1->Release();
    }
    factory->Release();
    return adapter_ != nullptr;
  }

  IDXGIAdapter3* adapter_ = nullptr;
  bool attempted_ = false;
};

#endif  // XE_PLATFORM_WIN32

// Mean of the slowest 1% of per-tick readings (at least one tick), which is
// what surfaces stutter that an average would hide.
double OnePercentLow(std::vector<double> values) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  size_t count = std::max<size_t>(1, values.size() / 100);
  double sum = std::accumulate(values.begin(), values.begin() + count, 0.0);
  return sum / double(count);
}

}  // namespace

PerfMonitor& PerfMonitor::Get() {
  static PerfMonitor instance;
  return instance;
}

PerfMonitor::~PerfMonitor() { Shutdown(); }

void PerfMonitor::Start() {
  if (!cvars::perf_monitor) {
    return;
  }
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    return;
  }
  log_path_ = xe::filesystem::GetExecutableFolder() / "perf_session.csv";
  if (cvars::perf_log_to_file) {
    // Each session owns the file outright - drop whatever the last run left.
    std::error_code ec;
    std::filesystem::remove(log_path_, ec);
    WriteCsvHeader();
    XELOGI("PerfMonitor: logging to {}", log_path_.string());
  }
  stop_requested_.store(false, std::memory_order_release);
  thread_ = std::thread(&PerfMonitor::ThreadMain, this);
}

void PerfMonitor::Shutdown() {
  if (!running_.exchange(false)) {
    return;
  }
  stop_requested_.store(true, std::memory_order_release);
  if (thread_.joinable()) {
    thread_.join();
  }
}

PerfSample PerfMonitor::live() const {
  std::lock_guard<std::mutex> lock(data_mutex_);
  return live_;
}

std::vector<PerfSample> PerfMonitor::samples() const {
  std::lock_guard<std::mutex> lock(data_mutex_);
  return samples_;
}

void PerfMonitor::WriteCsvHeader() {
  std::ofstream out(log_path_, std::ios::trunc);
  if (!out) {
    XELOGE("PerfMonitor: cannot write {}", log_path_.string());
    return;
  }
  out << "time_s,interval_s,frames_total,frames,fps_avg,fps_min,fps_max,"
         "fps_1pct_low,frame_ms_avg,cpu_proc_pct,cpu_sys_pct,ram_ws_mb,"
         "ram_priv_mb,vram_used_mb,vram_budget_mb\n";
}

void PerfMonitor::AppendCsvRow(const PerfSample& s) {
  if (!cvars::perf_log_to_file) {
    return;
  }
  std::ofstream out(log_path_, std::ios::app);
  if (!out) {
    return;
  }
  char line[512];
  std::snprintf(
      line, sizeof(line),
      "%.1f,%.1f,%llu,%llu,%.2f,%.2f,%.2f,%.2f,%.2f,%.1f,%.1f,%.1f,%.1f,%.1f,"
      "%.1f\n",
      s.uptime_seconds, s.interval_seconds,
      static_cast<unsigned long long>(s.frames_total),
      static_cast<unsigned long long>(s.frames_in_interval), s.fps_avg,
      s.fps_min, s.fps_max, s.fps_1pct_low, s.frame_ms_avg,
      s.cpu_process_percent, s.cpu_system_percent, s.ram_working_set_mb,
      s.ram_private_mb, s.vram_used_mb, s.vram_budget_mb);
  out << line;
}

void PerfMonitor::ThreadMain() {
#if XE_PLATFORM_WIN32
  CpuUsageSampler cpu_sampler;
  VideoMemorySampler vram_sampler;
#endif  // XE_PLATFORM_WIN32

  const auto started_at = Clock::now();
  auto last_tick = started_at;
  auto interval_started = started_at;
  uint64_t last_frames_total = frames_total_.load(std::memory_order_relaxed);
  uint64_t interval_start_frames = last_frames_total;
  std::vector<double> tick_fps;

  while (!stop_requested_.load(std::memory_order_acquire)) {
    // Sleep in small slices so shutdown does not wait out a whole second.
    for (int i = 0; i < 10 && !stop_requested_.load(std::memory_order_acquire);
         ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (stop_requested_.load(std::memory_order_acquire)) {
      break;
    }

    const auto now = Clock::now();
    const double tick_seconds = SecondsBetween(last_tick, now);
    last_tick = now;
    if (tick_seconds <= 0.0) {
      continue;
    }

    const uint64_t frames_now = frames_total_.load(std::memory_order_relaxed);
    const uint64_t tick_frames = frames_now - last_frames_total;
    last_frames_total = frames_now;
    const double tick_fps_value = double(tick_frames) / tick_seconds;
    tick_fps.push_back(tick_fps_value);

    PerfSample current;
    current.uptime_seconds = SecondsBetween(started_at, now);
    current.interval_seconds = tick_seconds;
    current.frames_total = frames_now;
    current.frames_in_interval = tick_frames;
    current.fps_avg = tick_fps_value;
    current.fps_min = tick_fps_value;
    current.fps_max = tick_fps_value;
    current.fps_1pct_low = tick_fps_value;
    current.frame_ms_avg = tick_fps_value > 0.0 ? 1000.0 / tick_fps_value : 0.0;
#if XE_PLATFORM_WIN32
    cpu_sampler.Sample(&current.cpu_process_percent,
                       &current.cpu_system_percent);
    SampleMemory(&current.ram_working_set_mb, &current.ram_private_mb);
    vram_sampler.Sample(&current.vram_used_mb, &current.vram_budget_mb);
#endif  // XE_PLATFORM_WIN32

    {
      std::lock_guard<std::mutex> lock(data_mutex_);
      live_ = current;
    }

    const double interval_seconds = SecondsBetween(interval_started, now);
    const double wanted =
        double(std::max<uint64_t>(1, cvars::perf_monitor_interval));
    if (interval_seconds < wanted) {
      continue;
    }

    PerfSample row = current;
    row.interval_seconds = interval_seconds;
    row.frames_in_interval = frames_now - interval_start_frames;
    row.fps_avg = double(row.frames_in_interval) / interval_seconds;
    row.fps_min = *std::min_element(tick_fps.begin(), tick_fps.end());
    row.fps_max = *std::max_element(tick_fps.begin(), tick_fps.end());
    row.fps_1pct_low = OnePercentLow(tick_fps);
    row.frame_ms_avg = row.fps_avg > 0.0 ? 1000.0 / row.fps_avg : 0.0;

    {
      std::lock_guard<std::mutex> lock(data_mutex_);
      samples_.push_back(row);
    }
    AppendCsvRow(row);

    interval_started = now;
    interval_start_frames = frames_now;
    tick_fps.clear();
  }
}

}  // namespace xe
