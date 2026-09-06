/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/portal/hardware_portal.h"
#include "xenia/base/logging.h"

namespace xe {
namespace hid {

HardwarePortal::HardwarePortal() : Portal() {
  libusb_init(&context_);
  OpenDevice();
}

HardwarePortal::~HardwarePortal() {
  if (handle_) {
    CloseDevice();
  }

  libusb_exit(context_);
}

bool HardwarePortal::IsConnected() { return handle_ != nullptr; }

void HardwarePortal::OpenDevice() {
  if (!context_ || handle_) {
    return;
  }

  // Allow only one portal device at the time.
  for (const auto& entry : kPortalVendorProductIdList) {
    libusb_device_handle* handle = libusb_open_device_with_vid_pid(
        context_, entry.vendor_id, entry.product_id);
    if (!handle) {
      continue;
    }

    // No-op on Windows, but keeps the device usable if this is ever built
    // for a platform with a kernel HID driver bound to the portal.
    libusb_set_auto_detach_kernel_driver(handle, 1);

    const int claim_result = libusb_claim_interface(handle, 0);
    if (claim_result != LIBUSB_SUCCESS) {
      // Almost always the stock HID driver still owning the interface;
      // the fix is installing libusb/WinUSB over it with Zadig.
      XELOGE(
          "Portal: found {} ({:04X}:{:04X}) but could not claim it: {}. "
          "Install the libusb driver for it with Zadig.",
          entry.name, entry.vendor_id, entry.product_id,
          libusb_error_name(claim_result));
      libusb_close(handle);
      continue;
    }

    handle_ = handle;
    XELOGI("Portal: using {} ({:04X}:{:04X}) over USB.", entry.name,
           entry.vendor_id, entry.product_id);
    return;
  }

  XELOGW(
      "Portal: no supported portal found over USB. Plug one in, or set "
      "toypad_emulation = true to use the emulated ToyPad instead.");
}

void HardwarePortal::CloseDevice() {
  if (!handle_) {
    return;
  }

  libusb_release_interface(handle_, 0);
  libusb_close(handle_);
  handle_ = nullptr;
}

X_STATUS HardwarePortal::ReadInternal(std::span<uint8_t> data,
                                      int32_t& read_count) {
  if (!handle_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  const int result = libusb_interrupt_transfer(
      handle_, read_endpoint, data.data(), static_cast<int>(data.size()),
      &read_count, timeout);

  switch (result) {
    case LIBUSB_ERROR_NO_DEVICE:
      // Drop the dead handle, otherwise OpenDevice() would see a non-null
      // handle_ and refuse to reconnect when the portal is plugged back in.
      CloseDevice();
      return X_ERROR_DEVICE_NOT_CONNECTED;
    case LIBUSB_ERROR_TIMEOUT:
      return X_ERROR_SUCCESS;
    default:
      break;
  }

  if (result < 0) {
    XELOGW("Portal[Read] returned error: {:08X}", result);
    return X_ERROR_FUNCTION_FAILED;
  }
  return X_ERROR_SUCCESS;
}

X_STATUS HardwarePortal::WriteInternal(std::span<uint8_t> data) {
  if (!handle_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  const int result = libusb_interrupt_transfer(
      handle_, write_endpoint, data.data(), static_cast<int>(data.size()),
      nullptr, timeout);

  if (result == LIBUSB_ERROR_NO_DEVICE) {
    CloseDevice();
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  if (result < 0) {
    XELOGW("Portal[Write] returned error: {:08X}", result);
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  return X_ERROR_SUCCESS;
};

void HardwarePortal::OnDeviceArrival() { OpenDevice(); };

void HardwarePortal::OnDeviceRemoval() { CloseDevice(); };

}  // namespace hid
}  // namespace xe
