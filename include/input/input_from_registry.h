#pragma once
#include <string>
#include "input/input_factory.h"   // brings in InputConfig, UsbOptions, RtspOptions, FileOptions

// Auto-detect the first available V4L2 USB camera device (e.g., /dev/video0).
// Returns empty string if no device is found.
std::string autoDetectUsbDeviceV4L2();

// Build an InputConfig from a single registry value:
//  - "usb_camera" | "usb" | "camera" | "/dev/videoX" | "0"/"1" → USB
//  - "rtsp://..." / "rtsps://..."                        → RTSP
//  - "/path/to/file" | "./file" | "../file"              → File
InputConfig make_input_from_registry_value(const std::string& raw);

