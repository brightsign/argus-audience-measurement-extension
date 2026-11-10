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
#include <condition_variable>
#include <deque>
#include <map>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace argus_display { class DisplayFrameSource; struct DisplayFrame; }

namespace argus_pop {

// Strict ISO-8601 -> UTC milliseconds. Requires an explicit offset ("Z" or +-HH[:MM]) so a wall-clock time is never guessed.
// A "+" that a query string turned into a space is accepted ("2026-11-07T14:00:00 01:00").
bool parse_iso8601_utc(const std::string& s, int64_t& utc_ms, std::string& err);
std::string format_iso8601_utc(int64_t utc_ms);        // YYYY-MM-DDTHH:MM:SS.mmmZ

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
    std::string storage_root;                                          // visual_memory.pop.storage_root: own directory (pop.db, refs, evidence). Empty = visual_memory.storage_root
    bool        allow_schema_upgrade = false;                      // upgrade an existing schema-1 pop.db to schema 2 (needed for resilience_grpc evidence)
    std::string capture_backend = "brightscript_screenshot";       // or "resilience_grpc" (OS rendered frames, needs visual_memory.display.enabled)
    int         grpc_before_ms = 300;                              // accept a display frame this long before the target offset ...
    int         grpc_after_ms = 800;                               // ... or this long after it
    int         grpc_max_uncertainty_ms = 1500;                    // reject a frame whose time uncertainty exceeds this
    int         grpc_start_guard_ms = 400;                         // the earliest possible display moment of a frame must be this long after the play_started event
    int         grpc_end_guard_ms = 400;                           // ... and its capture must be this long before the play end (transition-adjacent frames are not evidence)
    int         grpc_ready_timeout_ms = 4000;                      // scheduling waits this long for a fresh display frame before refusing (require_ready)
    int         grpc_prewarm_s = 120;                              // keep the OS producer awake this long between scheduling and the player starting
    int         grpc_max_dense_captures = 60;                      // upper bound for capture_every_s sessions
    visualmemory::VerifyThresholds thresholds;
    std::string device_timezone;                                       // visual_memory.pop.device_timezone: IANA name of the player's timezone (informational default for the time-search UI; all storage is UTC)

    // Phase 2: about one display frame per second while a registered advertisement session plays (visual_memory.pop.history). OFF by default.
    // Needs capture_backend resilience_grpc (the OS rendered frames); never records outside a session; separate from the three verification checkpoints.
    struct History {
        bool        enabled = false;
        int         interval_ms = 1000;               // 250..10000
        int         phase_ms = 0;                     // target offsets are phase + k*interval: 0 makes the 3, 15, 27 s verification checkpoints coincide with history targets (their frames are reused)
        int         window_ms = 300;                  // a frame is taken within +/- this of the target
        int         commit_delay_ms = 1500;           // a staged frame is committed this long after its capture, so a session end can still cancel it
        int         queue_capacity = 8;               // bounded hand-over to the writer thread
        int         max_frames_per_session = 600;
        uint64_t    max_bytes = 256ull * 1024 * 1024; // quota for history JPEGs (separate from evidence_max_bytes and from the camera store)
        int         jpeg_retention_days = 7;          // JPEG removed after this long (vector and metadata stay)
        int         metadata_retention_days = 30;     // row + vector removed after this long
        uint64_t    min_free_bytes = 256ull * 1024 * 1024;   // recording pauses below this much free space on the pop storage filesystem
        uint64_t    max_rows = 50000;                  // bound on stored history rows: the exact search scans every row (about 5 us/row on the host)
        uint64_t    max_pending_embeddings = 300;     // un-embedded rows allowed before new samples are refused
        int         embed_batch = 2;                  // vectors computed per NPU turn
    } history;
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
    // profile "screenshot" (default) reads refs_incoming/<id>/, "display_letterbox" reads refs_incoming/<id>.display_letterbox/ and stores the
    // references under the hidden media row <id>.display_letterbox (used only for OS-rendered-frame evidence).
    bool build_references(const std::string& media_id, nlohmann::json& out, int& http, std::string& err, bool replace = false,
                          const std::string& profile = "screenshot");
    // Optional OS rendered-frame source (capture_backend "resilience_grpc"). Not owned.
    void set_display(argus_display::DisplayFrameSource* d) { display_ = d; }
    bool verify(const std::string& session_id, nlohmann::json& out, int& http, std::string& err);
    // Visual Search source "BrightSign Display": searches only the persisted display-capture evidence with a reference JPEG embedded by the
    // same provider as D5. `q` carries top_k, min_similarity (default: thresholds.match_sim), start, end, media_id, method, require_image.
    bool search_display(const std::vector<uint8_t>& jpeg, const std::map<std::string, std::string>& q, nlohmann::json& out, int& http, std::string& err);

    // Phase 2 display history (all no-ops unless cfg().history.enabled)
    nlohmann::json history_status();
    nlohmann::json history_frames(const std::string& session_id, int limit, int offset);
    bool history_enabled() const { return cfg_.history.enabled && display_ != nullptr; }
    void history_flush_for_test();

    // Time-based lookup of stored display-output frames (read-only; uses stored timestamps and vectors, never the model).
    // nearest: one frame closest to utc_ms within max_diff_ms (found=false + an explanation when there is none).
    nlohmann::json display_nearest(int64_t utc_ms, int64_t max_diff_ms, const std::string& session_id);
    // search: every PoP session overlapping [utc_ms-window_ms, utc_ms+window_ms] with its stored frames; max_frames per session.
    nlohmann::json display_time_search(int64_t utc_ms, int64_t window_ms, int max_frames);
    nlohmann::json display_time_info();                 // device timezone, retained range, limits                      // drain the writer queue and embed everything pending (tests / shutdown)

    nlohmann::json session_detail(const std::string& id, bool* found);
    nlohmann::json session_json(const visualmemory::PopSession& s);
    std::string export_csv(const visualmemory::SessionFilter& f);
    void prune();                           // independent retention of evidence images

    // exposed for tests: process one JSONL line as if read at wall-clock `read_utc_ms`
    void process_line(const std::string& line, int64_t read_utc_ms);

private:
    nlohmann::json time_frame_json(const visualmemory::DisplayFrameRef& f, int64_t requested_ms, const visualmemory::FrameSim* sim);
    nlohmann::json time_session_json(const visualmemory::PopSession& s, int64_t requested_ms);
    void loop();
    void grpc_loop();                                  // selects display frames at the capture offsets of active sessions
    void grpc_collect(const std::string& sid, bool final_pass, int64_t end_utc_ms);
    // Stores one piece of evidence (frame == nullptr records a failed capture). false = embedding provider busy, try again.
    bool grpc_record(const std::string& sid, int idx, int64_t target_ms, const std::string& why, const argus_display::DisplayFrame* frame, int64_t play_start_utc);
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

    // capture_backend = resilience_grpc
    struct GrpcPlan {
        std::vector<int64_t> offsets_ms;
        std::vector<char> done;
        int64_t slot_ms = 0, play_start_utc = 0, created_ms = 0;
        bool started = false, break_capture = false, ready_at_start = false, prewarm_released = false;
    };
    // ---- display history (Phase 2) ----
    struct HistStaged { int k = 0; int target_off = 0; std::shared_ptr<const std::string> jpeg; int64_t capture_utc = 0, capture_mono = 0, uncertainty = 0; uint64_t seq = 0; int provider = 0; };
    struct HistSession {
        std::string expected;
        int64_t start_utc = 0, start_mono = 0, slot_ms = 0;
        int next_k = 0, enqueued = 0, missed = 0, duplicates = 0, dropped_after_end = 0;
        uint64_t last_seq = 0;
        bool ready_at_start = false;
        std::deque<HistStaged> staged;
    };
    struct HistJob { bool finalize = false; visualmemory::HistoryFrameIn in; std::string sid, played; int64_t end_utc = 0; };
    void history_begin(const std::string& sid, int64_t start_utc, int64_t slot_ms, bool ready_at_start);
    void history_tick();                                // grpc thread: samples due targets, commits staged frames
    void history_end(const std::string& sid, int64_t end_utc);   // session_end: final pass, drops frames from after the end, queues the finalize
    void history_abort(const std::string& sid);
    void history_sample(HistSession& hs, const std::string& sid, int64_t now_mono, bool final_pass, int64_t end_utc);
    bool history_offer(HistJob j);
    void history_loop();                                // writer + embedder thread
    std::mutex hist_mu_;                                // guards hist_sessions_
    std::map<std::string, HistSession> hist_sessions_;
    std::mutex hq_mu_;
    std::condition_variable hq_cv_;
    std::deque<HistJob> hq_;
    std::thread hist_th_;
    struct HistCounters {
        std::atomic<uint64_t> targets{0}, staged{0}, queued{0}, written{0}, reused_checkpoint{0}, embedded{0}, missed_no_frame{0}, duplicate_frames{0},
            dropped_queue_full{0}, dropped_after_end{0}, rejected_invalid{0}, rejected_duplicate{0}, session_limit{0}, backlog_refused{0},
            write_failed{0}, sqlite_failed{0}, embed_failed{0}, paused_low_space{0}, embed_busy{0};
        std::atomic<int64_t> last_write_ms_x100{0}, max_write_ms_x100{0}, last_embed_ms_x100{0}, last_commit_utc_ms{0};
    } hc_;
    std::string hist_last_error_;
    argus_display::DisplayFrameSource* display_ = nullptr;
    std::mutex plan_mu_;
    std::map<std::string, GrpcPlan> plans_;
    std::thread grpc_th_;
};

} // namespace argus_pop

#endif
