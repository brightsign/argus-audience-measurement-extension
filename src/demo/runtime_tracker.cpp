#include "demo/runtime_tracker.h"
#include "metrics/log_global.h"

#include <3rdparty/nlohmann/json.hpp>
#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>

using json = nlohmann::json;

RuntimeTracker::RuntimeTracker(std::string state_file_path, int limit_seconds)
    : state_file_path_(std::move(state_file_path)) {
    state_.demo_limit_seconds = limit_seconds;
}

RuntimeTracker::~RuntimeTracker() {
    flush();
}

bool RuntimeTracker::initialize() {
    if (load_state()) {
        LG_DEBUG("Demo mode: State file loaded: %d seconds cumulative (%d days)",
                 state_.cumulative_seconds, state_.cumulative_seconds / 86400);
        return true;
    }

    // Create a fresh state file.
    state_ = create_new_state();
    if (!save_state()) {
        LG_WARN("Demo mode: Failed to create state file: %s", state_file_path_.c_str());
        // Continue in memory only -- worst case we lose up to 15 minutes on crash.
        return true;
    }

    LG_INFO("Demo mode: New state file created at %s", state_file_path_.c_str());
    return true;
}

bool RuntimeTracker::is_expired() const {
    if (!state_.demo_mode_enabled) {
        return false;
    }
    return state_.cumulative_seconds >= state_.demo_limit_seconds;
}

int RuntimeTracker::remaining_seconds() const {
    int remaining = state_.demo_limit_seconds - state_.cumulative_seconds;
    return remaining > 0 ? remaining : 0;
}

void RuntimeTracker::update(int elapsed_seconds) {
    if (is_expired()) {
        return;
    }

    state_.cumulative_seconds += elapsed_seconds;
    accumulated_since_flush_ += elapsed_seconds;

    if (accumulated_since_flush_ >= flush_interval_seconds) {
        accumulated_since_flush_ = 0;
        if (!save_state()) {
            LG_WARN("Demo mode: Failed to write state file: %s (state tracked in memory only)",
                    state_file_path_.c_str());
        } else {
            LG_DEBUG("Demo mode: State file updated: %d seconds cumulative",
                     state_.cumulative_seconds);
        }
    }
}

void RuntimeTracker::flush() {
    if (!save_state()) {
        LG_WARN("Demo mode: Final flush failed: %s", state_file_path_.c_str());
    }
}

int RuntimeTracker::cumulative_seconds() const {
    return state_.cumulative_seconds;
}

const std::string& RuntimeTracker::first_started_utc() const {
    return state_.first_started_utc;
}

bool RuntimeTracker::load_state() {
    std::ifstream ifs(state_file_path_);
    if (!ifs.is_open()) {
        return false;
    }

    try {
        json j;
        ifs >> j;

        State loaded;
        loaded.version           = j.value("version", 1);
        loaded.first_started_utc = j.value("first_started_utc", std::string{});
        loaded.cumulative_seconds = j.value("cumulative_seconds", 0);
        loaded.last_updated_utc  = j.value("last_updated_utc", std::string{});
        loaded.demo_mode_enabled = j.value("demo_mode_enabled", true);

        // Preserve the limit set at construction; only use the file value when
        // it is non-zero so that we do not reset it to zero accidentally.
        int file_limit = j.value("demo_limit_seconds", 0);
        loaded.demo_limit_seconds = (file_limit > 0) ? file_limit : state_.demo_limit_seconds;

        state_ = loaded;
        return true;
    } catch (const std::exception& ex) {
        LG_WARN("Demo mode: State file corrupted (%s), starting fresh", ex.what());
        return false;
    }
}

bool RuntimeTracker::save_state() const {
    const std::string now_ts = current_utc_timestamp();

    try {
        json j;
        j["version"]            = state_.version;
        j["first_started_utc"]  = state_.first_started_utc;
        j["cumulative_seconds"] = state_.cumulative_seconds;
        j["last_updated_utc"]   = now_ts;
        j["demo_mode_enabled"]  = state_.demo_mode_enabled;
        j["demo_limit_seconds"] = state_.demo_limit_seconds;

        std::ofstream ofs(state_file_path_);
        if (!ofs.is_open()) {
            return false;
        }
        ofs << j.dump(2) << '\n';
        return ofs.good();
    } catch (const std::exception&) {
        return false;
    }
}

RuntimeTracker::State RuntimeTracker::create_new_state() const {
    State s;
    s.first_started_utc  = current_utc_timestamp();
    s.last_updated_utc   = s.first_started_utc;
    s.demo_limit_seconds = state_.demo_limit_seconds;
    return s;
}

std::string RuntimeTracker::current_utc_timestamp() {
    auto now  = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc_tm{};
    gmtime_r(&time, &utc_tm);

    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc_tm);
    return buf;
}
