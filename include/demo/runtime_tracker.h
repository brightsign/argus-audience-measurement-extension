#pragma once

#include <string>

// Tracks cumulative runtime across sessions to enforce a demo mode time limit.
// State is persisted to disk every 15 minutes to survive reboots and restarts.
// After the limit is reached, frame processing must stop immediately.
class RuntimeTracker {
public:
    static constexpr int default_limit_seconds = 1296000;  // 15 days
    static constexpr int flush_interval_seconds = 900;     // 15 minutes

    RuntimeTracker(std::string state_file_path,
                   int limit_seconds = default_limit_seconds);
    ~RuntimeTracker();

    // Load state from disk or create a new state file if one does not exist.
    // Returns false only on unrecoverable failures such as permission denied.
    bool initialize();

    // Returns true if cumulative runtime has exceeded the configured limit.
    bool is_expired() const;

    // Returns seconds remaining before expiration (0 if already expired).
    int remaining_seconds() const;

    // Accumulate elapsed_seconds into cumulative total; flush to disk if the
    // flush interval has elapsed since the last write.
    void update(int elapsed_seconds);

    // Force immediate write of current state to disk.
    void flush();

    int cumulative_seconds() const;
    const std::string& first_started_utc() const;

private:
    struct State {
        int version{1};
        std::string first_started_utc;
        int cumulative_seconds{0};
        std::string last_updated_utc;
        bool demo_mode_enabled{true};
        int demo_limit_seconds{default_limit_seconds};
    };

    bool load_state();
    bool save_state() const;
    State create_new_state() const;
    static std::string current_utc_timestamp();

    std::string state_file_path_;
    State state_;
    int accumulated_since_flush_{0};
};
