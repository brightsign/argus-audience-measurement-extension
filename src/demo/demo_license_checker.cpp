#include "demo/demo_license_checker.h"
#include "metrics/log_global.h"

#include <3rdparty/nlohmann/json.hpp>
#include <cstdio>
#include <ctime>
#include <fstream>

using json = nlohmann::json;

DemoLicenseChecker::DemoLicenseChecker(const std::string& license_file_path)
    : file_path_(license_file_path) {}

bool DemoLicenseChecker::check() {
    if (expired_) {
        return true;
    }

    std::ifstream ifs(file_path_);
    if (!ifs.is_open()) {
        LG_CRIT("Demo mode: license file absent or unreadable: %s — treating as expired",
                file_path_.c_str());
        expired_ = true;
        return true;
    }

    json j;
    try {
        ifs >> j;
    } catch (const std::exception& ex) {
        LG_CRIT("Demo mode: failed to parse license file %s: %s — treating as expired",
                file_path_.c_str(), ex.what());
        expired_ = true;
        return true;
    }

    int version = j.value("version", -1);
    if (version != 1) {
        LG_CRIT("Demo mode: unsupported license file version %d — treating as expired", version);
        expired_ = true;
        return true;
    }

    if (!j.contains("expires_utc") || !j["expires_utc"].is_string()) {
        LG_CRIT("Demo mode: license file missing 'expires_utc' field — treating as expired");
        expired_ = true;
        return true;
    }

    expires_utc_ = j["expires_utc"].get<std::string>();

    std::time_t expiry_time{};
    if (!parse_iso8601_utc(expires_utc_, expiry_time)) {
        LG_CRIT("Demo mode: 'expires_utc' is not a valid UTC datetime (%s) — treating as expired",
                expires_utc_.c_str());
        expired_ = true;
        return true;
    }

    std::time_t now = std::time(nullptr);
    if (now >= expiry_time) {
        expired_ = true;
        return true;
    }

    return false;
}

bool DemoLicenseChecker::is_expired() const {
    return expired_;
}

const std::string& DemoLicenseChecker::expires_utc() const {
    return expires_utc_;
}

bool DemoLicenseChecker::parse_iso8601_utc(const std::string& s, std::time_t& out) const {
    // Expected format: YYYY-MM-DDTHH:MM:SSZ
    if (s.size() < 20 || s.back() != 'Z') {
        return false;
    }

    std::tm tm{};
    int matched = std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2dZ",
                              &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                              &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
    if (matched != 6) {
        return false;
    }

    tm.tm_year -= 1900;
    tm.tm_mon  -= 1;
    tm.tm_isdst = 0;

    // timegm interprets the struct tm as UTC (POSIX extension, Linux/glibc).
    out = timegm(&tm);
    return out != static_cast<std::time_t>(-1);
}
