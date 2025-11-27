#include "input/registry_helper.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>

const std::string RegistryHelper::DAEMON_NAME = "bsext-gaze";

// -------- public --------

std::string RegistryHelper::readExtensionValue(const std::string& key) {
    const std::string command = "registry extension " + key;
    return executeCommand(command);
}

std::string RegistryHelper::getUdpPublishRate() {
    const std::string key = DAEMON_NAME + "-udp-publish-rate";
    std::string publishRate = readExtensionValue(key);
    std::printf("DEBUG: Registry key: '%s'\n", key.c_str());
    std::printf("DEBUG: Raw registry result: '%s' (length: %zu)\n",
                publishRate.c_str(), publishRate.length());

    if (publishRate.empty()) {
        std::printf("DEBUG: Registry value is empty, using default '10'\n");
        publishRate = "10";
    } else {
        std::printf("DEBUG: Using registry value: '%s'\n", publishRate.c_str());
    }
    return publishRate;
}

std::string RegistryHelper::getVideoDevice() {
    const std::string key = "bsext-gaze-video-device";
    std::string val = readExtensionValue(key);
    std::printf("DEBUG: Registry key: '%s'\n", key.c_str());
    std::printf("DEBUG: Raw registry result: '%s' (length: %zu)\n",
                val.c_str(), val.length());

    if (val.empty()) {
        std::printf("DEBUG: Registry value empty; defaulting to 'usb_camera'\n");
        return std::string("usb_camera");
    }

    const std::string low = toLower(val);
    std::printf("DEBUG: Lowercased value: '%s'\n", low.c_str());

    if (looksLikeUsbToken(low)) {
        std::printf("DEBUG: Detected as USB token, returning 'usb_camera'\n");
        return "usb_camera"; // normalize
    }
    if (looksLikeRtsp(low)) {
        std::printf("DEBUG: Detected as RTSP URL, returning original: '%s'\n", val.c_str());
        return val;          // keep original URL
    }
    if (looksLikePath(val)) {
        std::printf("DEBUG: Detected as file path, returning: '%s'\n", val.c_str());
        return val;          // absolute/relative path
    }
    if (looksLikeCameraIndex(low)) {
        std::printf("DEBUG: Detected as camera index, returning: '/dev/video%s'\n", low.c_str());
        return "/dev/video" + low; // "0" -> "/dev/video0"
    }

    // Also allow explicit /dev/videoX device nodes
    if (startsWith(val, "/dev/video")) {
        std::printf("DEBUG: Detected as /dev/video device, returning: '%s'\n", val.c_str());
        return val;
    }

    std::fprintf(stderr,
        "DEBUG: Unrecognized video device value '%s'; falling back to 'usb_camera'\n",
        val.c_str());
    return std::string("usb_camera");
}

 std::string RegistryHelper::findWorkingCameraDevice() {
    std::vector<std::string> devices;
    if (DIR* dir = opendir("/dev")) {
        if (dirent* e; (e = readdir(dir)) != nullptr) {
            do {
                std::string name = e->d_name;
                if (name.rfind("video", 0) == 0) devices.emplace_back("/dev/" + name);
            } while ((e = readdir(dir)) != nullptr);
        }
        closedir(dir);
    }
    std::sort(devices.begin(), devices.end());
    
    if (devices.empty()) {
        std::printf("DEBUG: findWorkingCameraDevice: no /dev/video* nodes found\n");
        return "";
    }
    
    std::printf("DEBUG: findWorkingCameraDevice: probing %zu candidates:\n", devices.size());
    for (const auto& dev : devices) {
        std::printf("DEBUG: findWorkingCameraDevice:  candidate %s\n", dev.c_str());
    }
    
    for (const auto& dev : devices) {
        // Check read permission
        if (access(dev.c_str(), R_OK) != 0) {
            std::printf("DEBUG: findWorkingCameraDevice: %s - permission denied\n", dev.c_str());
            continue;
        }
        
        // Try to open with OpenCV
        cv::VideoCapture cap(dev, cv::CAP_V4L2);
        if (!cap.isOpened()) {
            std::printf("DEBUG: findWorkingCameraDevice: %s - did not open (cv::VideoCapture)\n", dev.c_str());
            continue;
        }
        
        // Try to grab one frame
        cv::Mat f;
        if (!cap.read(f) || f.empty()) {
            std::printf("DEBUG: findWorkingCameraDevice: %s - opened but no frame (or empty frame)\n", dev.c_str());
            continue;
        }
        
        std::printf("DEBUG: findWorkingCameraDevice: %s WORKS (%dx%d frames)\n", dev.c_str(), f.cols, f.rows);
        return dev;
    }
    
    std::printf("DEBUG: findWorkingCameraDevice: no working device found\n");
    return "";
}
// -------- private --------

std::string RegistryHelper::executeCommand(const std::string& command) {
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"), pclose);
    if (!pipe) {
        std::printf("DEBUG: Failed to open pipe for command: %s\n", command.c_str());
        return "";
    }

    char buffer[256];
    std::string result;
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        result += buffer;
    }

    // Trim whitespace (spaces, tabs, CR, LF)
    result.erase(std::remove_if(result.begin(), result.end(),
                [](unsigned char c){ return std::isspace(c); }),
                result.end());
    return result;
}

std::string RegistryHelper::toLower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool RegistryHelper::startsWith(const std::string& s, const char* pfx) {
    return s.rfind(pfx, 0) == 0;
}
bool RegistryHelper::looksLikePath(const std::string& s) {
    return !s.empty() && (s[0]=='/' || startsWith(s,"./") || startsWith(s,"../"));
}
bool RegistryHelper::looksLikeRtsp(const std::string& s) {
    return startsWith(s,"rtsp://") || startsWith(s,"rtsps://");
}
bool RegistryHelper::looksLikeUsbToken(const std::string& s) {
    return (s=="usb_camera" || s=="usb" || s=="camera");
}
bool RegistryHelper::looksLikeCameraIndex(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

