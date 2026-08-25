/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_PORTAL_EMULATED_TOYPAD_H_
#define XENIA_HID_PORTAL_EMULATED_TOYPAD_H_

#include <array>
#include <atomic>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "xenia/base/mutex.h"
#include "xenia/hid/portal/portal.h"

namespace xe {
namespace hid {

constexpr size_t kToypadFigureCount = 7;
constexpr size_t kToypadTagSize = 0x2D * 0x04;  // 180 bytes, NTAG213 dump
// Upper bound for queued toypad replies; oldest frames are dropped beyond it.
constexpr size_t kMaxQueuedResponses = 256;

struct ToypadFigure {
  std::string file_path;  // optional .bin backing file, empty = in-memory only
  std::array<uint8_t, kToypadTagSize> data{};
  uint8_t index = 255;  // 1-based slot index as reported to the game, 255=free
  uint8_t pad = 255;    // 1=centre, 2=left, 3=right
  uint32_t id = 0;
  void Save();
};

// Emulated LEGO Dimensions ToyPad. Protocol logic is a port of the
// dimensions_toypad implementation from RPCS3 (rpcs3/Emu/Io/Dimensions.cpp,
// GPLv2) as used by the RPCS3-Seamless-Toypad-Build companion project.
// A loopback TCP listener accepts LOAD/REMOVE/MOVE commands from the
// LegoToypad companion app (same wire contract as the Cemu/RPCS3 forks).
class EmulatedToypad final : public Portal {
 public:
  EmulatedToypad();
  ~EmulatedToypad() override;

  bool IsConnected() override { return true; }
  void OnDeviceArrival() override {}
  void OnDeviceRemoval() override {}

  // True while the companion app's picker overlay is open; gamepad input
  // should not reach the game.
  static bool IsPickerInputActive();

  // Listener-driven state changes (called from the listener thread).
  uint32_t LoadFigure(const std::array<uint8_t, kToypadTagSize>& buf,
                      std::string file_path, uint8_t pad, uint8_t index,
                      bool lock);
  bool RemoveFigure(uint8_t pad, uint8_t index, bool full_remove, bool lock);
  bool TempRemove(uint8_t index);
  bool CancelRemove(uint8_t index);
  bool MoveFigure(uint8_t pad, uint8_t index, uint8_t old_pad,
                  uint8_t old_index);

 private:
  void OpenDevice() override {}
  void CloseDevice() override {}
  X_STATUS ReadInternal(std::span<uint8_t> data, int32_t& read_count) override;
  X_STATUS WriteInternal(std::span<uint8_t> data) override;

  void HandleCommand(const uint8_t* frame, size_t frame_size);

  // Protocol helpers.
  static uint8_t GenerateChecksum(const std::array<uint8_t, 32>& data,
                                  uint32_t num_of_bytes);
  static void GetBlankResponse(uint8_t type, uint8_t sequence,
                               std::array<uint8_t, 32>& reply_buf);
  void GenerateRandomNumber(const uint8_t* buf, uint8_t sequence,
                            std::array<uint8_t, 32>& reply_buf);
  void InitializeRNG(uint32_t seed);
  uint32_t GetNext();
  void GetChallengeResponse(const uint8_t* buf, uint8_t sequence,
                            std::array<uint8_t, 32>& reply_buf);
  void QueryBlock(uint8_t index, uint8_t page,
                  std::array<uint8_t, 32>& reply_buf, uint8_t sequence);
  void WriteBlock(uint8_t index, uint8_t page, const uint8_t* to_write_buf,
                  std::array<uint8_t, 32>& reply_buf, uint8_t sequence);
  void GetModel(const uint8_t* buf, uint8_t sequence,
                std::array<uint8_t, 32>& reply_buf);

  static std::array<uint8_t, 8> Decrypt(
      const uint8_t* buf, std::optional<std::array<uint8_t, 16>> key);
  static std::array<uint8_t, 8> Encrypt(
      const uint8_t* buf, std::optional<std::array<uint8_t, 16>> key);
  static std::array<uint8_t, 16> GenerateFigureKey(
      const std::array<uint8_t, kToypadTagSize>& buf);
  static uint32_t Scramble(const std::array<uint8_t, 7>& uid, uint8_t count);
  static std::array<uint8_t, 4> Randomize(const std::vector<uint8_t>& key,
                                          uint8_t count);
  static uint32_t GetFigureId(const std::array<uint8_t, kToypadTagSize>& buf);
  ToypadFigure& GetFigureByIndex(uint8_t index);

  void PushResponse(std::array<uint8_t, 32> frame);

  // Listener.
  void ListenerRun(uint16_t port);
  void HandleClient(uintptr_t client_socket);
  void PickerWatchRun();

  xe_mutex state_lock_;
  std::array<ToypadFigure, kToypadFigureCount> figures_{};
  std::queue<std::array<uint8_t, 32>> responses_;

  uint32_t random_a_ = 0;
  uint32_t random_b_ = 0;
  uint32_t random_c_ = 0;
  uint32_t random_d_ = 0;

  // Frame wrapping detected from the game's writes. -1 = not yet known,
  // 0 = raw 0x55/0x56 frames, >0 = number of prefix bytes before 0x55.
  int frame_offset_ = -1;
  uint8_t frame_prefix_byte_ = 0;

  std::thread listener_thread_;
  std::thread picker_thread_;
  std::atomic<bool> listener_running_{false};
  std::atomic<uintptr_t> listen_socket_{~uintptr_t(0)};
};

}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_PORTAL_EMULATED_TOYPAD_H_
