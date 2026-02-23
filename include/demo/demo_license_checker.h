#pragma once

#include <ctime>
#include <string>

// Enforces a demo expiration date by reading a JSON license file every time
// check() is called. If the file is absent or malformed the demo is treated as
// expired (fail-closed). Returns false only when the file is present, valid,
// and the expiration timestamp is still in the future.
class DemoLicenseChecker {
public:
    explicit DemoLicenseChecker(const std::string& license_file_path);

    // Read the license file and compare expires_utc to the current wall-clock
    // time. Returns true if expired (file absent, malformed, or past expiry).
    // Returns false if the expiry is still in the future.
    // Once this returns true, subsequent calls always return true.
    bool check();

    // True after the first check() call that returned true.
    bool is_expired() const;

    // The expires_utc string read from the file, or empty if the file was
    // absent or malformed on the last check() call.
    const std::string& expires_utc() const;

private:
    std::string file_path_;
    bool expired_{false};
    std::string expires_utc_;

    // Parse an ISO 8601 UTC string "YYYY-MM-DDTHH:MM:SSZ" into a time_t.
    // Returns false on any parse failure.
    bool parse_iso8601_utc(const std::string& s, std::time_t& out) const;
};
