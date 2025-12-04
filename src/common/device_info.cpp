#include "common/device_info.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <vector>
#include <unistd.h>  // gethostname

namespace device_info {

std::string get_device_id_from_mac(const std::string& interface) noexcept {
  try {
    // Try reading MAC address from /sys/class/net/<interface>/address
    std::string mac_path = "/sys/class/net/" + interface + "/address";
    std::ifstream mac_file(mac_path);
    
    if (mac_file.is_open()) {
      std::string mac_addr;
      std::getline(mac_file, mac_addr);
      mac_file.close();
      
      if (!mac_addr.empty()) {
        // Remove colons and convert to uppercase
        std::string clean_mac;
        for (char c : mac_addr) {
          if (c != ':' && c != '\n' && c != '\r') {
            clean_mac += std::toupper(static_cast<unsigned char>(c));
          }
        }
        
        // Format as "BS-AABBCCDDEEFF"
        if (clean_mac.length() == 12) {
          return "BS-" + clean_mac;
        }
      }
    }
  } catch (...) {
    // Ignore errors, will try other methods
  }
  
  return "";
}

std::string get_device_id(const std::string& fallback) noexcept {
  // Method 1: Try MAC address from common interfaces
  const std::vector<std::string> interfaces = {
    "eth0", "enp194s0", "enp0s3", "ens33", "wlan0", "wlp195s0"
  };
  
  for (const auto& iface : interfaces) {
    std::string device_id = get_device_id_from_mac(iface);
    if (!device_id.empty()) {
      return device_id;
    }
  }
  
  // Method 2: Try reading DMI product serial
  try {
    std::ifstream serial_file("/sys/devices/virtual/dmi/id/product_serial");
    if (serial_file.is_open()) {
      std::string serial;
      std::getline(serial_file, serial);
      serial_file.close();
      
      if (!serial.empty() && serial != "None" && serial != "To Be Filled By O.E.M.") {
        // Remove whitespace
        serial.erase(std::remove_if(serial.begin(), serial.end(), ::isspace), serial.end());
        if (!serial.empty()) {
          return "BS-" + serial;
        }
      }
    }
  } catch (...) {
    // Ignore errors
  }
  
  // Method 4: Try reading CPU serial from /proc/cpuinfo
  try {
    std::ifstream cpuinfo("/proc/cpuinfo");
    if (cpuinfo.is_open()) {
      std::string line;
      while (std::getline(cpuinfo, line)) {
        if (line.find("Serial") == 0) {
          size_t colon = line.find(':');
          if (colon != std::string::npos) {
            std::string serial = line.substr(colon + 1);
            // Trim whitespace
            serial.erase(0, serial.find_first_not_of(" \t"));
            serial.erase(serial.find_last_not_of(" \t\n\r") + 1);
            
            if (!serial.empty() && serial != "0000000000000000") {
              return "BS-" + serial;
            }
          }
        }
      }
      cpuinfo.close();
    }
  } catch (...) {
    // Ignore errors
  }
  
  // Method 5: Try hostname
  try {
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
      std::string hn(hostname);
      if (!hn.empty() && hn != "localhost") {
        // Convert to uppercase and remove invalid characters
        std::string clean_hn;
        for (char c : hn) {
          if (std::isalnum(static_cast<unsigned char>(c)) || c == '-') {
            clean_hn += std::toupper(static_cast<unsigned char>(c));
          }
        }
        if (!clean_hn.empty()) {
          return "BS-" + clean_hn;
        }
      }
    }
  } catch (...) {
    // Ignore errors
  }
  
  // All methods failed, return fallback
  return fallback;
}

} // namespace device_info
