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

    if (looksLikeUsbToken(low))      return "usb_camera"; // normalize
    if (looksLikeRtsp(low))          return val;          // keep original URL
    if (looksLikePath(val))          return val;          // absolute/relative path
    if (looksLikeCameraIndex(low))   return "/dev/video" + low; // "0" -> "/dev/video0"

    // Also allow explicit /dev/videoX device nodes
    if (startsWith(val, "/dev/video")) return val;

    std::fprintf(stderr,
        "DEBUG: Unrecognized video device value '%s'; falling back to 'usb_camera'\n",
        val.c_str());
    return std::string("usb_camera");
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

