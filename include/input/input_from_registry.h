#pragma once
#include <string>
#include "input/input_factory.h"   // brings in InputConfig, UsbOptions, RtspOptions, FileOptions

// Build an InputConfig from a single registry value:
//  - "usb_camera" | "usb" | "camera" | "/dev/videoX" | "0"/"1" → USB
//  - "rtsp://..." / "rtsps://..."                        → RTSP
//  - "/path/to/file" | "./file" | "../file"              → File
InputConfig make_input_from_registry_value(const std::string& raw);

