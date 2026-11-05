// SPDX-License-Identifier: MIT
// D5 Proof-of-Play service used by vm_api_server: owns the PopStore, runs the playback-event adapter (tails the
// player application's JSONL event file, stamps wall-clock time, ingests display screenshots and verifies finished
// sessions) and implements scheduling / reference-library operations. The adapter is the ONLY code that knows the
// BrightScript event format; libvisualmemory stays player-independent.
#ifndef ARGUS_POP_SERVICE_H
#define ARGUS_POP_SERVICE_H

#include <3rdparty/nlohmann/json.hpp>

#include "visualmemory/embedding_provider.h"
#include "visualmemory/pop.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace argus_pop {

struct PopCfg {
    bool        enabled = false;                                   // visual_memory.pop.enabled
    std::string events_path = "/storage/sd/pop/events.jsonl";      // JSONL written by the player application
    std::string jobs_dir = "/storage/sd/pop/jobs";                 // where scheduled plays are handed to the player application
    std::string sd_from = "SD:/", sd_to = "/storage/sd/";          // BrightScript path prefix -> Linux path
    std::string source_id = "main_display";
    int         poll_ms = 200;
    uint64_t    evidence_max_bytes = 512ull * 1024 * 1024;
    int         evidence_retention_days = 30;
    bool        allow_test_jobs = false;                           // play a media other than the expected one, interrupt, break capture
    visualmemory::VerifyThresholds thresholds;
};

// Runs `fn` with an embedding provider (single-flight with the other NPU users). false + why when unavailable.
using ProviderRunner = std::function<bool(const std::function<void(visualmemory::EmbeddingProvider&)>& fn, std::string& why)>;

class PopService {
public:
    PopService(PopCfg cfg, std::string storage_root, ProviderRunner run);
    ~PopService();
    bool start(std::string& err);          // opens pop.db and starts the adapter thread
    void stop();
    bool running() const { return running_.load(); }

    visualmemory::PopStore& store() { return *store_; }
    const PopCfg& cfg() const { return cfg_; }
    nlohmann::json status();
    nlohmann::json system_json();           // read-only /proc + NPU + thermal snapshot for performance validation

    // scheduling: records the expectation and hands a job to the player application. Returns false + http code.
    bool schedule(const nlohmann::json& body, nlohmann::json& out, int& http, std::string& err);
    // ingest <root>/pop/refs_incoming/<media_id>/<offset_ms>.jpg into the reference library.
    bool build_references(const std::string& media_id, nlohmann::json& out, int& http, std::string& err, bool replace = false);
    bool verify(const std::string& session_id, nlohmann::json& out, int& http, std::string& err);

    nlohmann::json session_detail(const std::string& id, bool* found);
    nlohmann::json session_json(const visualmemory::PopSession& s);
    std::string export_csv(const visualmemory::SessionFilter& f);
    void prune();                           // independent retention of evidence images

    // exposed for tests: process one JSONL line as if read at wall-clock `read_utc_ms`
    void process_line(const std::string& line, int64_t read_utc_ms);

private:
    void loop();
    bool ingest_capture(const nlohmann::json& j, int64_t utc_ms, const std::string& sid, bool ok);
    std::string map_path(const std::string& p) const;
    void save_state();
    void load_state();

    PopCfg cfg_;
    std::string root_;
    ProviderRunner run_;
    std::unique_ptr<visualmemory::PopStore> store_;
    std::thread th_;
    std::atomic<bool> running_{false}, stop_{false};
    // adapter state
    std::mutex mu_;
    uint64_t offset_ = 0;
    int64_t anchor_ms_ = INT64_MAX;       // utc - mono, smallest seen in this boot (latency-free estimate)
    uint64_t lines_ = 0, events_ = 0, errors_ = 0, boots_ = 0;
    int64_t last_event_utc_ms_ = 0, last_prune_ms_ = 0;
    std::string last_error_;
    uint64_t file_size_ = 0;
};

} // namespace argus_pop

#endif
