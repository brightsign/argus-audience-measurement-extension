#pragma once
#include <string>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>
#include <memory>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

#include "opencv2/core/core.hpp"
#include "opencv2/imgproc/imgproc.hpp"
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/videoio.hpp>


// Minimal helper to read BrightSign registry values via the `registry extension` CLI.
// Matches your previous main.cpp snippet and adds getVideoDevice().
//
// Usage:
//   const std::string raw = RegistryHelper::getVideoDevice();  // "usb_camera" | "rtsp://..." | "/path"
//   InputConfig ic = make_input_from_registry_value(raw);

class RegistryHelper {
private:
    static const std::string DAEMON_NAME;   // e.g., "bsext-gaze" (used by other keys like udp rate)

public:
    // Generic reader (already used in your old main.cpp)
    static std::string readExtensionValue(const std::string& key);

    // Existing example you had (kept for compatibility)
    static std::string getUdpPublishRate();

    // NEW: the key you mentioned — returns either:
    //   "usb_camera"  (or "usb"/"camera")
    //   "rtsp://..."  (or "rtsps://...")
    //   "/path/to/file"  (absolute/relative)
    //   "/dev/videoX" or numeric "0","1" (if user sets device explicitly)
    // Falls back to "usb_camera" if empty/invalid.
    static std::string getVideoDevice();
    static std::string findWorkingCameraDevice();

private:
    static std::string executeCommand(const std::string& command);

    // small utils (private)
    static std::string toLower(std::string s);
    static bool startsWith(const std::string& s, const char* pfx);
    static bool looksLikePath(const std::string& s);
    static bool looksLikeRtsp(const std::string& s);
    static bool looksLikeUsbToken(const std::string& s);
    static bool looksLikeCameraIndex(const std::string& s);
};

