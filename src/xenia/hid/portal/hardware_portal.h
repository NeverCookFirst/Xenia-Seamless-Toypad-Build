/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_PORTAL_HARDWARE_PORTAL_H_
#define XENIA_HID_PORTAL_HARDWARE_PORTAL_H_

#include <array>
#include "xenia/hid/portal/portal.h"

#include "third_party/libusb/libusb/libusb.h"

namespace xe {
namespace hid {

struct PortalDeviceId {
  uint16_t vendor_id;
  uint16_t product_id;
  const char* name;
};

// The LEGO Dimensions ToyPad ("LEGO READER V2.10") is a PDP device and uses
// the same 0E6F:0241 pair on every platform it shipped for. It needs a
// libusb-compatible driver on Windows, which is what Zadig installs.
constexpr std::array<PortalDeviceId, 3> kPortalVendorProductIdList = {
    PortalDeviceId{0x0E6F, 0x0241, "LEGO Dimensions ToyPad"},
    PortalDeviceId{0x1430, 0x1F17, "Skylanders Portal of Power"},
    PortalDeviceId{0x24C6, 0xFA00, "Disney Infinity Base"}};

class HardwarePortal final : public Portal {
 public:
  HardwarePortal();
  ~HardwarePortal() override;

  virtual bool IsConnected() override;

  virtual void OnDeviceArrival() override;
  virtual void OnDeviceRemoval() override;

 private:
  virtual void OpenDevice() override;
  virtual void CloseDevice() override;
  virtual X_STATUS ReadInternal(std::span<uint8_t> data,
                                int32_t& read_count) override;
  virtual X_STATUS WriteInternal(std::span<uint8_t> data) override;

  // Endpoint addresses differ per portal (the Skylanders one answers on
  // 0x81/0x02, the LEGO ToyPad does not), so they are read out of the device's
  // own descriptor rather than assumed. A wrong guess shows up as
  // LIBUSB_ERROR_NOT_FOUND on every transfer.
  bool FindEndpoints(libusb_device_handle* handle);

  const uint16_t timeout = 100;

  uint8_t read_endpoint_ = 0;
  uint8_t write_endpoint_ = 0;
  int interface_number_ = 0;

  // The Xbox 360 does not hand the portal a bare command frame: it wraps it as
  // <prefix> <length> 55 ..., the same shape EmulatedToypad detects. Real
  // hardware speaks the unwrapped 0x55 frame, so the wrapper is stripped on the
  // way out and put back on the way in. -1 = no write seen yet.
  int frame_offset_ = -1;
  uint8_t frame_prefix_byte_ = 0;

  libusb_context* context_ = nullptr;
  libusb_device_handle* handle_ = nullptr;
};

}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_PORTAL_HARDWARE_PORTAL_H_
