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
};

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

class PopStore {
public:
    explicit PopStore(PopConfig cfg);
    ~PopStore();
    PopStore(const PopStore&) = delete;
    PopStore& operator=(const PopStore&) = delete;

    bool open(std::string& err);                       // creates/migrates <root>/pop.db
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
    // Applies the canonical event vocabulary: stores the event and advances the session state. Unknown sessions
    // are created ad hoc (expected_media_id from the event, else the played one).
    bool apply_event(const PopEventIn& e, std::string& err);
    std::vector<PopEvent> list_events(const std::string& session_id);

    // display-capture evidence: stores the image under pop/evidence/YYYY/MM/DD/HH/<source>/ (hourly partition)
    bool add_evidence(EmbeddingProvider& provider, const EvidenceIn& in, Evidence& out, std::string& err);
    std::vector<Evidence> list_evidence(const std::string& session_id);
    bool evidence_jpeg(int64_t evidence_id, std::vector<uint8_t>& out);
    static std::string partition_path(const std::string& source, int64_t utc_ms, const std::string& file_name);

    // verification: loads the session, its events/evidence and every media's references, runs verify_playback,
    // stores the result (replacing the previous one of the session).
    bool verify_session(const std::string& session_id, const VerifyThresholds& t, Verification& out, std::string& err);
    bool get_verification(const std::string& session_id, Verification& out);
    std::string get_verification_json(const std::string& session_id);     // "" if none

    // independent retention of evidence images: removes files older than `older_than_ms` (0 = skip) and, when
    // max_bytes > 0, the oldest ones beyond that total. Rows and scores stay (image_available becomes false).
    uint64_t prune_evidence(int64_t older_than_ms, uint64_t max_bytes);

    std::string root() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 64-bit DCT perceptual hash of a JPEG (0 + false on decode failure) and the Hamming distance of two hashes.
bool phash_of_jpeg(const std::vector<uint8_t>& jpeg, uint64_t& out, std::string& err);
int  phash_distance(uint64_t a, uint64_t b);

} // namespace visualmemory

#endif // VISUALMEMORY_POP_H
