/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/portal/hardware_portal.h"
#include <algorithm>
#include <array>
#include <cstring>

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

bool HardwarePortal::FindEndpoints(libusb_device_handle* handle) {
  read_endpoint_ = 0;
  write_endpoint_ = 0;
  interface_number_ = 0;

  libusb_config_descriptor* config = nullptr;
  if (libusb_get_active_config_descriptor(libusb_get_device(handle), &config) !=
      LIBUSB_SUCCESS) {
    return false;
  }

  for (uint8_t i = 0; i < config->bNumInterfaces && !read_endpoint_; i++) {
    const libusb_interface& iface = config->interface[i];
    for (int a = 0; a < iface.num_altsetting; a++) {
      const libusb_interface_descriptor& alt = iface.altsetting[a];
      uint8_t in = 0, out = 0;
      for (uint8_t e = 0; e < alt.bNumEndpoints; e++) {
        const libusb_endpoint_descriptor& ep = alt.endpoint[e];
        if ((ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) !=
            LIBUSB_TRANSFER_TYPE_INTERRUPT) {
          continue;
        }
        if (ep.bEndpointAddress & LIBUSB_ENDPOINT_IN) {
          if (!in) {
            in = ep.bEndpointAddress;
          }
        } else if (!out) {
          out = ep.bEndpointAddress;
        }
      }
      if (in && out) {
        read_endpoint_ = in;
        write_endpoint_ = out;
        interface_number_ = alt.bInterfaceNumber;
        break;
      }
    }
  }

  libusb_free_config_descriptor(config);
  if (!read_endpoint_) {
    return false;
  }

  XELOGI("Portal: interface {}, interrupt endpoints in {:02X} out {:02X}",
         interface_number_, read_endpoint_, write_endpoint_);
  return true;
}

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

    if (!FindEndpoints(handle)) {
      XELOGE("Portal: {} ({:04X}:{:04X}) exposes no interrupt endpoint pair.",
             entry.name, entry.vendor_id, entry.product_id);
      libusb_close(handle);
      continue;
    }

    const int claim_result = libusb_claim_interface(handle, interface_number_);
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

  libusb_release_interface(handle_, interface_number_);
  libusb_close(handle_);
  handle_ = nullptr;
  read_endpoint_ = 0;
  write_endpoint_ = 0;
}

X_STATUS HardwarePortal::ReadInternal(std::span<uint8_t> data,
                                      int32_t& read_count) {
  if (!handle_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  std::array<uint8_t, kPortalBufferSize> frame{};
  const int result = libusb_interrupt_transfer(
      handle_, read_endpoint_, frame.data(), static_cast<int>(frame.size()),
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

  // Hand the guest the frame back in the shape it writes in. Length is the
  // header's payload count plus the type, length and checksum bytes.
  std::array<uint8_t, kPortalBufferSize> out{};
  if (frame_offset_ > 0) {
    const size_t len = std::min<size_t>(size_t(frame[1]) + 3, out.size());
    out[0] = frame_prefix_byte_;
    out[1] = static_cast<uint8_t>(len);
    std::memcpy(&out[2], frame.data(), std::min(len, out.size() - 2));
  } else {
    out = frame;
  }

  const size_t count = std::min(out.size(), data.size());
  std::memcpy(data.data(), out.data(), count);
  read_count = static_cast<int32_t>(count);
  return X_ERROR_SUCCESS;
}

X_STATUS HardwarePortal::WriteInternal(std::span<uint8_t> data) {
  if (!handle_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Find the 0x55 command frame. The Xbox 360 transport prepends a short
  // wrapper (0x0B <len> ...) that the portal itself knows nothing about, so
  // send it what it speaks and remember the wrapper for the reply.
  size_t offset = 0;
  const size_t scan_limit = std::min<size_t>(4, data.size());
  for (; offset < scan_limit; offset++) {
    if (data[offset] == 0x55) {
      break;
    }
  }
  if (offset == scan_limit) {
    XELOGW("Portal[Write] no 0x55 frame found; dropping it");
    return X_ERROR_SUCCESS;
  }
  if (frame_offset_ != static_cast<int>(offset)) {
    frame_offset_ = static_cast<int>(offset);
    frame_prefix_byte_ = offset > 0 ? data[0] : 0;
    XELOGI("Portal: guest frame offset {} (prefix {:02X})", offset,
           frame_prefix_byte_);
  }

  std::array<uint8_t, kPortalBufferSize> frame{};
  std::memcpy(frame.data(), data.data() + offset,
              std::min(data.size() - offset, frame.size()));

  const int result = libusb_interrupt_transfer(
      handle_, write_endpoint_, frame.data(), static_cast<int>(frame.size()),
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
