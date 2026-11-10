// SPDX-License-Identifier: MIT
// Proof-of-Play store and verification engine (D5).
//
// WHAT THIS IS: evidence that an advertisement (a media asset) was shown on the PRESENTATION OUTPUT of a player. It
// combines (1) playback events reported by the player's own application and (2) screenshots of the display output,
// embedded with the same MobileNetV2 model as Visual Memory and compared with a reference library of the expected
// media. The audience camera is never evidence of what was shown.
//
// WHAT THIS IS NOT: a certificate of audited impressions. A "Playback corroborated" status means the reported events
// and the sampled screenshots both agree with the expected media; it says nothing about whether anyone watched. Scores
// are cosine similarities, not probabilities.
//
// Storage: <root>/pop.db (own versioned schema, PRAGMA user_version) plus image files under <root>/pop/. Like the
// D4 object index it is a separate SQLite file: Argus keeps sole ownership of visual_memory.db.
//
// Independent of Argus, BrightScript and any player API: this header only needs the other libvisualmemory headers and
// the standard library. How a player produces events/screenshots is the business of an adapter outside the library;
// the canonical event vocabulary (apply_event) is documented in docs/VISUAL_MEMORY_PROOF_OF_PLAY.md.
#ifndef VISUALMEMORY_POP_H
#define VISUALMEMORY_POP_H

#include "visualmemory/embedding_provider.h"
#include "visualmemory/types.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace visualmemory {

// ---- status -----------------------------------------------------------------------------------------------------
enum class PopStatus {
    Scheduled,             // an expectation exists, nothing has been reported yet
    PlaybackReported,      // the player reported playback; visual evidence absent or inconclusive
    VisualMatch,           // screenshots match the expected media, but the player's events do not complete the story
    PlaybackCorroborated,  // complete playback events AND screenshots matching the expected media
    Mismatch,              // screenshots show different content than expected
    Incomplete,            // playback stopped early / failed (shorter than the expected slot)
    CaptureUnsupported     // playback reported but no screenshot could be obtained: visual verification unavailable
};
const char* to_string(PopStatus s);                 // "scheduled", "playback_reported", ...
bool parse_pop_status(const std::string& s, PopStatus& out);
const char* pop_status_label(PopStatus s);          // human label, e.g. "Playback corroborated"

// ---- records ----------------------------------------------------------------------------------------------------
struct MediaAsset {
    std::string media_id;                  // stable identifier, [A-Za-z0-9._-]{1,64}
    std::string display_name;
    std::string kind = "file";             // "file" | "rtsp"
    std::string uri;                       // path or URL as the player is told to play it
    std::string sha256;                    // hash of the media file ("" for a live stream)
    std::string codec;
    double      duration_s = 0;            // actual media duration (0 for a live stream)
    double      slot_s = 30;               // expected time on screen per scheduled play; completeness is judged against it
    int         width = 0, height = 0;
    double      fps = 0;
    int64_t     created_utc_ms = 0, updated_utc_ms = 0;
    uint64_t    reference_frames = 0;      // filled by list/get
};

struct ReferenceFrame {
    int64_t     ref_id = 0;
    std::string media_id;
    int         offset_ms = 0;             // position in the media the frame was taken from
    uint64_t    phash = 0;
    std::string path;                      // relative to <root>
    int         width = 0, height = 0;
};

struct PopSession {
    std::string session_id;
    std::string job_id;
    std::string expected_media_id;         // what the schedule says should play
    std::string played_media_id;           // what the player says it played ("" until reported)
    std::string source = "main_display";   // evidence source: the presentation output
    std::string state = "scheduled";       // scheduled | running | ended
    std::string end_reason;                // slot_end | ended | interrupted | play_failed | start_timeout | ...
    int64_t     scheduled_utc_ms = 0, start_utc_ms = 0, end_utc_ms = 0;
    int64_t     actual_play_ms = -1;       // reported by the player (monotonic clock), -1 unknown
    int64_t     slot_ms = 30000;
    int         captures_requested = 0;
    int64_t     created_utc_ms = 0;
};

struct PopEventIn {                        // the canonical event, already time-stamped by the adapter
    std::string session_id;
    std::string type;                      // see docs: session_scheduled_run, play_requested, play_started, play_ended,
                                           // play_stopped, play_error, video_event, capture_ok, capture_failed, session_end
    int64_t     utc_ms = 0;                // wall clock assigned by the adapter
    int64_t     mono_ms = 0;               // player monotonic clock (as reported)
    std::string job_id, played_media_id, expected_media_id, reason;
    int64_t     actual_play_ms = -1;
    int         captures_requested = -1;
    std::string detail_json = "{}";        // the raw event, kept for audit
};

struct PopEvent {
    int64_t event_id = 0;
    std::string session_id, type, detail_json;
    int64_t mono_ms = 0, utc_ms = 0;
};

struct EvidenceIn {
    std::string session_id;
    int         idx = 0;                   // 1-based capture number within the session
    int         target_offset_ms = 0;      // requested offset from the playback start
    int         actual_offset_ms = 0;      // when the capture really ran
    std::string status = "ok";             // "ok" | "failed"
    std::string failure_reason;
    std::string source = "main_display";
    int64_t     utc_ms = 0;
    int         took_ms = 0;
    std::string capture_method = "brightscript_screenshot";   // or "resilience_grpc" (OS rendered frames)
    std::string provenance;                // e.g. "weston_vop2_writeback (provider 7)"; approximate composed display output
    int         uncertainty_ms = -1;       // time uncertainty of utc_ms (-1 = not applicable / unknown)
    std::vector<uint8_t> jpeg;             // image bytes when status == "ok"
};

struct Evidence {
    int64_t     evidence_id = 0;
    std::string session_id;
    int         idx = 0, target_offset_ms = 0, actual_offset_ms = 0;
    std::string status, failure_reason, source;
    std::string path;                      // relative to <root>; "" when failed
    uint64_t    phash = 0;
    std::string sha256;
    int         width = 0, height = 0;
    uint64_t    bytes = 0;
    int         took_ms = 0;
    int64_t     utc_ms = 0;
    bool        embedded = false;
    bool        image_available = false;   // file retained (retention may remove it; the scores are kept)
    std::string capture_method = "brightscript_screenshot", provenance;
    int         uncertainty_ms = -1;
    // Phase 2: a search hit may be a display-history sample (table display_history_frames) instead of a verification checkpoint.
    std::string role = "verification_checkpoint";     // verification_checkpoint | history_sample
    int64_t     history_id = 0;                        // display_history_frames.frame_id when role == history_sample
    int         sec_idx = 0;                           // history sample index inside the session
    std::string expected_media_id;                     // history samples only (as scheduled)
};

// Reference frames can be built for a second capture geometry (the OS rendered frames are letterboxed 640x360, the BrightScript
// screenshots are 640x400). They live in a hidden media row named <media_id> + this suffix and are used only when verifying evidence
// captured with the matching method.
inline const char* kDisplayProfileSuffix() { return ".display_letterbox"; }

// ---- verification -----------------------------------------------------------------------------------------------
struct VerifyThresholds {
    float  match_sim = 0.80f;              // a frame matches the expected media at or above this cosine similarity
    float  reject_sim = 0.60f;             // below this, the frame does not resemble the expected media at all
    float  margin = 0.05f;                 // another asset must beat the expected one by this much to call a mismatch
    int    max_phash_dist = 20;            // informational second signal (Hamming distance of 64-bit pHash)
    double min_play_ratio = 0.90;          // actual play time / slot length that counts as complete
    double min_match_fraction = 0.60;      // fraction of usable frames that must match
};

struct FrameScore {
    int         idx = 0;
    int64_t     evidence_id = 0;
    int         actual_offset_ms = 0;
    float       sim_expected = -1.0f;      // best cosine to any reference frame of the expected media
    int         best_ref_offset_ms = -1;   // where in the expected media that best reference sits
    int         phash_dist_expected = 64;  // smallest Hamming distance to an expected reference
    float       sim_other = -1.0f;         // best cosine to any OTHER registered media
    std::string best_other_media;
    int         best_other_ref_offset_ms = -1;   // offset of that other media's closest reference (diagnostic only; not used in any decision)
    std::string verdict;                   // "match" | "mismatch" | "inconclusive"
};

struct Verification {
    int64_t     verification_id = 0;
    std::string session_id, expected_media_id, best_media_id;
    PopStatus   status = PopStatus::Scheduled;
    std::string reason;
    int         frames_total = 0, frames_ok = 0, frames_matched = 0, frames_mismatched = 0, frames_failed = 0;
    double      mean_sim_expected = 0, min_sim_expected = 0, mean_phash_dist = 0;
    bool        playback_reported = false, playback_complete = false, session_ended = false;
    int64_t     actual_play_ms = -1, slot_ms = 0;
    std::vector<FrameScore> frames;
    std::string engine_version;
    int64_t     created_utc_ms = 0;
};

struct VerifyInput {
    std::string expected_media_id;
    bool        reported = false, ended = false;      // events: any playback reported / session_end seen
    std::string end_reason;
    int64_t     actual_play_ms = -1, slot_ms = 30000;
    int         captures_requested = 0, captures_failed = 0;
    struct Ev  { int idx = 0; int64_t evidence_id = 0; int actual_offset_ms = 0; std::vector<float> emb; uint64_t phash = 0; };
    struct Ref { std::string media_id; int offset_ms = 0; std::vector<float> emb; uint64_t phash = 0; };
    std::vector<Ev>  evidence;                         // successfully captured, embedded frames
    std::vector<Ref> refs;                             // references of every registered media (L2-normalised)
};
// Pure function (no I/O): the decision logic, unit-testable with fabricated vectors.
Verification verify_playback(const VerifyInput& in, const VerifyThresholds& t);
std::string verification_to_json(const Verification& v);   // compact JSON (frames included)

// ---- store ------------------------------------------------------------------------------------------------------
struct PopConfig {
    std::string root_directory;            // same directory as visual_memory.db
    int         busy_timeout_ms = 3000;
    bool        sync_full = true;
    // An existing schema-1 pop.db is NOT upgraded unless this is set: it keeps working exactly as before (legacy mode, screenshot evidence
    // only) and the file is not altered by opening it. Schema 2 adds the evidence capture-method columns (OS rendered frames).
    bool        allow_schema_upgrade = false;
};

struct PopSummary {
    bool open = false;
    int  schema_version = 0;
    uint64_t media = 0, reference_frames = 0, sessions = 0, events = 0, evidence = 0, evidence_bytes = 0, verifications = 0;
    uint64_t db_bytes = 0;
    int64_t  oldest_session_utc_ms = 0, newest_session_utc_ms = 0;
};

struct SessionFilter {
    std::string expected_media_id, status;             // status = PopStatus string, "" any
    int64_t start_utc_ms = 0, end_utc_ms = 0;
};

// ---- display-evidence search (BrightSign Display source of Visual Search) ----------------------------------------
// Exact cosine search over the persisted, embedded display-capture evidence. Only rows that exist in display_capture_evidence
// (status ok, embedded, same model as the query) are candidates; nothing is generated, estimated or read from the camera store.
struct EvidenceSearchOptions {
    int     top_k = 10;                                     // 1..50
    bool    has_min_similarity = true;
    float   min_similarity = 0.80f;                          // hits below it are not "matches" (the closest one is still reported)
    int64_t start_utc_ms = 0, end_utc_ms = 0;               // capture-time window, 0 = open
    std::string capture_method = "resilience_grpc";         // "" = any capture method
    std::string media_id;                                    // "" = any advertisement (matches the session's expected or played media)
    bool    require_image = false;                           // only evidence whose JPEG is still retained
    std::string session_id;                                  // "" = any playback session
    std::string role;                                        // "" = checkpoints + history, "verification_checkpoint", "history_sample"
    std::function<bool()> cancel;                            // polled during the scan
};
struct EvidenceHit {
    Evidence evidence;
    float    similarity = 0;
};
struct EvidenceCoverage {
    uint64_t rows_total = 0;                                 // every evidence row (any status)
    uint64_t rows_searchable = 0;                            // ok + embedded + the query's model + the filters
    uint64_t rows_other_model = 0;                           // ok + embedded but produced by a different model (not comparable)
    uint64_t images_retained = 0;                            // searchable rows whose JPEG is still on disk
    uint64_t sessions = 0;                                   // distinct sessions among the searchable rows
    int64_t  oldest_utc_ms = 0, newest_utc_ms = 0;           // capture-time range of the searchable rows
    uint64_t checkpoint_rows = 0, history_rows = 0;          // split of rows_searchable (history rows reusing a checkpoint are counted once)
};
struct EvidenceSearchResult {
    std::vector<EvidenceHit> hits;                           // similarity >= min (or all, when no minimum), best first, at most top_k
    bool     has_closest = false;                            // the best candidate overall, reported even when below the minimum
    EvidenceHit closest;
    uint64_t scanned = 0, skipped_invalid = 0;
    double   elapsed_ms = 0;
    double   embed_ms = 0;          // decode + preprocess + NPU embedding of the query
    double   scan_ms = 0;           // SQLite read + exact dot-product scan
    bool     cancelled = false;
    EvidenceCoverage coverage;
};

// ---- display history (Phase 2): about one rendered display frame per second during registered playback ----------------------------------------
// Separate, additive table display_history_frames inside the same pop.db. It is NOT read by the verification engine (which only looks at
// display_capture_evidence), so history can never change a verdict. user_version stays 2: an older binary simply ignores the table.
struct HistoryFrameIn {
    std::string session_id;
    int         sec_idx = 0;                  // 0.. sample number inside the session (unique per session)
    int         target_offset_ms = 0, actual_offset_ms = 0;
    int64_t     utc_ms = 0, mono_ms = 0;      // frame capture time (upper bound of the display moment) and the daemon monotonic clock
    int         uncertainty_ms = -1;
    std::string source = "main_display", expected_media_id;
    std::string capture_method = "resilience_grpc", provenance;
    std::vector<uint8_t> jpeg;                // the OS-compressed bytes, stored unchanged
};
struct HistoryFrame {
    int64_t     frame_id = 0;
    std::string session_id;
    int         sec_idx = 0;
    int64_t     evidence_id = 0;              // != 0: this instant is also a verification checkpoint; the checkpoint's JPEG and vector are reused
    std::string role = "history_sample";      // history_sample | history_and_checkpoint
    std::string source, expected_media_id, played_media_id, capture_method, provenance, path, sha256, status = "ok";
    int64_t     utc_ms = 0, mono_ms = 0;
    int         target_offset_ms = 0, actual_offset_ms = 0, uncertainty_ms = -1, width = 0, height = 0;
    uint64_t    bytes = 0;
    bool        embedded = false, image_available = false;
    int         embed_attempts = 0;
};
struct HistoryLimits {
    int      max_frames_per_session = 600;
    uint64_t max_pending_embeddings = 300;    // un-embedded rows allowed before new samples are refused (backlog bound)
};
struct HistoryStats {
    bool     ready = false;
    uint64_t rows = 0, embedded = 0, pending_embed = 0, embed_failed = 0, linked_to_checkpoint = 0, images_retained = 0, image_bytes = 0, sessions = 0;
    int64_t  oldest_utc_ms = 0, newest_utc_ms = 0;
};
struct HistoryPruneResult { uint64_t images_removed = 0, rows_removed = 0, bytes_freed = 0; };

// ---- time-based lookup of stored display-output frames (read-only; no model, no RKNN) -------------------------------------------
// A "display frame" is a Weston/VOP2 frame (capture_method resilience_grpc) kept either as a Phase 2 history sample or as a D5
// verification checkpoint that has no history row. A checkpoint that history already links to is reported once, as the history row.
struct DisplayFrameRef {
    std::string kind;                         // history_sample | history_and_checkpoint | verification_checkpoint
    int64_t     frame_id = 0;                 // display_history_frames.frame_id (0 for a pure checkpoint)
    int64_t     evidence_id = 0;              // display_capture_evidence.evidence_id (0 for a pure history sample)
    int         index = 0;                    // sec_idx (history) or idx (checkpoint)
    std::string session_id, source, capture_method, provenance, expected_media_id;
    int64_t     utc_ms = 0;                   // capture instant, UTC
    int         actual_offset_ms = 0;         // relative to the player-reported play start of the session
    int         uncertainty_ms = -1;          // -1: the capture timestamp has no stated bound
    int         width = 0, height = 0;
    uint64_t    bytes = 0;
    std::string sha256;
    bool        embedded = false, image_available = false;
};
struct NearestDisplayFrame {
    bool    found = false;                    // a frame exists within max_diff_ms
    DisplayFrameRef frame;                    // the nearest frame at all (valid when has_nearest), also when it is outside the tolerance
    bool    has_nearest = false;
    int64_t diff_ms = 0;                      // recorded - requested (negative: the frame is earlier than the requested time)
    uint64_t frames_in_store = 0;             // display frames retained (history rows + unlinked checkpoints), all sessions
    int64_t oldest_utc_ms = 0, newest_utc_ms = 0;
};
struct FrameSimQuery { int64_t frame_id = 0, evidence_id = 0; };
struct FrameSim {
    bool    ok = false;
    float   sim = -1.0f;                      // best cosine to a reference frame of the session's expected media
    int     best_ref_offset_ms = -1, refs = 0;
    std::string reason;                       // why ok == false
};

class PopStore {
public:
    explicit PopStore(PopConfig cfg);
    ~PopStore();
    PopStore(const PopStore&) = delete;
    PopStore& operator=(const PopStore&) = delete;

    bool open(std::string& err);                       // creates <root>/pop.db (schema 2); upgrades a schema-1 file only if allow_schema_upgrade
    bool schema_v2() const;                            // false = legacy schema-1 database opened without upgrade
    bool is_open() const;
    void close();
    PopSummary summary();

    // reference library
    bool valid_media_id(const std::string& id) const;
    bool register_media(const MediaAsset& a, std::string& err);           // upsert (keeps the reference frames)
    bool get_media(const std::string& id, MediaAsset& out);
    std::vector<MediaAsset> list_media();
    bool delete_media(const std::string& id);                              // also removes its reference frames
    // Embed `jpeg`, compute its pHash, store a copy under pop/refs/<media>/ and the vector (replaces the same offset).
    bool add_reference(EmbeddingProvider& provider, const std::string& media_id, int offset_ms,
                       const std::vector<uint8_t>& jpeg, std::string& err);
    std::vector<ReferenceFrame> list_references(const std::string& media_id);
    bool clear_references(const std::string& media_id);                    // drops all reference frames of a media (files too)
    bool reference_jpeg(int64_t ref_id, std::vector<uint8_t>& out);

    // sessions and events
    bool schedule_session(const PopSession& s, std::string& err);         // creates (state "scheduled") or refuses a duplicate id
    bool get_session(const std::string& id, PopSession& out);
    std::vector<PopSession> list_sessions(const SessionFilter& f, int limit, int offset, uint64_t* total = nullptr);
    // Sessions whose play interval overlaps [from,to] (start..end; a session that never ended is bounded by start+slot) plus
    // sessions scheduled inside it that never started. Read-only.
    std::vector<PopSession> list_sessions_overlapping(int64_t from_utc_ms, int64_t to_utc_ms, int limit);
    // Applies the canonical event vocabulary: stores the event and advances the session state. Unknown sessions
    // are created ad hoc (expected_media_id from the event, else the played one).
    bool apply_event(const PopEventIn& e, std::string& err);
    std::vector<PopEvent> list_events(const std::string& session_id);

    // display-capture evidence: stores the image under pop/evidence/YYYY/MM/DD/HH/<source>/ (hourly partition)
    bool add_evidence(EmbeddingProvider& provider, const EvidenceIn& in, Evidence& out, std::string& err);
    std::vector<Evidence> list_evidence(const std::string& session_id);
    bool evidence_jpeg(int64_t evidence_id, std::vector<uint8_t>& out);
    static std::string partition_path(const std::string& source, int64_t utc_ms, const std::string& file_name);
    bool get_evidence(int64_t evidence_id, Evidence& out);

    // Display-evidence search. The query JPEG is decoded and embedded exactly like evidence and references are (same decoder, same provider
    // preprocessing, L2 normalisation). The scan is EXACT over the evidence BLOB column; pop.db has no ANN/vec0 table (see docs).
    bool search_evidence(EmbeddingProvider& provider, const std::vector<uint8_t>& query_jpeg, const EvidenceSearchOptions& o,
                         EvidenceSearchResult& out, std::string& err);
    EvidenceCoverage evidence_coverage(const EmbeddingSpec* model, const std::string& capture_method);   // model == nullptr: any model

    // verification: loads the session, its events/evidence and every media's references, runs verify_playback,
    // stores the result (replacing the previous one of the session).
    bool verify_session(const std::string& session_id, const VerifyThresholds& t, Verification& out, std::string& err);
    bool get_verification(const std::string& session_id, Verification& out);
    std::string get_verification_json(const std::string& session_id);     // "" if none

    // independent retention of evidence images: removes files older than `older_than_ms` (0 = skip) and, when
    // max_bytes > 0, the oldest ones beyond that total. Rows and scores stay (image_available becomes false).
    uint64_t prune_evidence(int64_t older_than_ms, uint64_t max_bytes);

    // ---- display history (additive table, created on demand; never created in a legacy schema-1 file) ----
    bool history_open(std::string& err);                    // CREATE TABLE IF NOT EXISTS ... (no PRAGMA user_version change)
    bool history_ready() const;
    // Validates the JPEG (complete, bounded, decodable header), writes it unchanged under pop/history/YYYY/MM/DD/HH/<source>/ (atomic) and inserts the
    // row. If a verification checkpoint of the same session already holds the very same frame (same capture time) no second copy is stored.
    // err == "duplicate" for an existing (session, sec_idx); "session limit" / "embedding backlog" when a bound is hit.
    bool add_history_frame(const HistoryFrameIn& in, const HistoryLimits& lim, HistoryFrame& out, std::string& err);
    // Embeds up to `max_rows` pending rows with `provider`. Returns the number embedded, or -1 with err.
    int  embed_pending_history(EmbeddingProvider& provider, int max_rows, std::string& err);
    uint64_t history_pending_embeddings();
    bool get_history(int64_t frame_id, HistoryFrame& out);
    std::vector<HistoryFrame> list_history(const std::string& session_id, int limit, int offset, uint64_t* total = nullptr);
    bool history_jpeg(int64_t frame_id, std::vector<uint8_t>& out);
    HistoryStats history_stats();
    // Time lookup. All three are read-only SELECTs that use the utc_ms indexes; the answer never depends on the model.
    bool nearest_display_frame(int64_t utc_ms, int64_t max_diff_ms, const std::string& session_id, NearestDisplayFrame& out);
    std::vector<DisplayFrameRef> display_frames_between(int64_t from_utc_ms, int64_t to_utc_ms, const std::string& session_id, int limit, bool* truncated = nullptr,
                                                        bool newest_first = false);   // newest_first: the LAST `limit` frames of the range, newest first
    std::vector<FrameSim> expected_similarity(const std::string& session_id, const std::vector<FrameSimQuery>& frames);
    // End of a session: records the played media on its rows and removes any frame captured after (end_utc_ms - guard_ms).
    uint64_t history_finalize_session(const std::string& session_id, int64_t end_utc_ms, int guard_ms, const std::string& played_media_id);
    // JPEGs older than jpeg_older_than_ms, and the oldest ones beyond max_bytes, are removed (row and vector stay); rows older than
    // meta_older_than_ms (and the oldest ones beyond max_rows) are deleted completely. 0 disables the respective limit.
    HistoryPruneResult prune_history(int64_t jpeg_older_than_ms, uint64_t max_bytes, int64_t meta_older_than_ms, uint64_t max_rows = 0);   // max_rows bounds the exact-scan size (oldest rows go first)

    std::string root() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 64-bit DCT perceptual hash of a JPEG (0 + false on decode failure) and the Hamming distance of two hashes.
bool phash_of_jpeg(const std::vector<uint8_t>& jpeg, uint64_t& out, std::string& err);
int  phash_distance(uint64_t a, uint64_t b);

// The validated display geometry (scripts/pop/make_reference_frames.sh, profile display_letterbox) applied to an arbitrary JPEG so that it is
// comparable with OS-rendered (Weston/VOP2) frames: stretch to 1920x1080, take the native top-left 1280x800 rectangle, scale it to 576x360 and
// centre it on a black 640x360 canvas. Output is a 640x360 JPEG. Area-averaging resampler (not Lanczos): close to, not bit-identical with, ffmpeg.
bool display_letterbox_jpeg(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, int& src_w, int& src_h, std::string& err);

} // namespace visualmemory

#endif // VISUALMEMORY_POP_H
