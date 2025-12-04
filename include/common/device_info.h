#ifndef DEVICE_INFO_H
#define DEVICE_INFO_H

#include <string>

namespace device_info {

/**
 * Get device identifier from system.
 * Attempts to read device serial/ID using multiple methods:
 * 1. Read from /sys/class/net/eth0/address (MAC address)
 * 2. Read from /sys/devices/virtual/dmi/id/product_serial
 * 3. Read from /proc/cpuinfo (Serial field)
 * 4. Generate from hostname
 * 
 * @param fallback Default value if all methods fail
 * @return Device identifier string (e.g., "BS-A1B2C3D4E5F6" from MAC)
 */
std::string get_device_id(const std::string& fallback = "BS-UNKNOWN") noexcept;

/**
 * Read MAC address from network interface and format as device ID
 * @param interface Network interface name (default: "eth0")
 * @return Device ID formatted as "BS-AABBCCDDEEFF" or empty string on failure
 */
std::string get_device_id_from_mac(const std::string& interface = "eth0") noexcept;

} // namespace device_info

#endif // DEVICE_INFO_H
