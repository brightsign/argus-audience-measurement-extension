// SPDX-License-Identifier: MIT
// ObjectFrameSource: where the D4 object indexer / ObjectIndex get their stored frames from.
//
//   camera  : Visual Memory frames (visual_memory.db, frames.id)              -> CameraObjectSource(VisualMemoryReader&)
//   display : Weston/VOP2 display-output frames kept by Proof of Play (pop.db) -> DisplayObjectSource(PopStore&)
//
// IMPORTANT: a camera frame id and a Proof-of-Play frame id come from DIFFERENT databases and are NOT globally unique.
// A stored frame is therefore always identified by the triple (source_type, kind, id):
//   camera  : kind ""            id = frames.id
//   display : kind "history"     id = display_history_frames.frame_id   (1 FPS history sample, also when it doubles as a checkpoint)
//             kind "checkpoint"  id = display_capture_evidence.evidence_id (a verification checkpoint with no history row)
// Nothing ever joins on id alone.
#ifndef VISUALMEMORY_OBJECT_FRAME_SOURCE_H
#define VISUALMEMORY_OBJECT_FRAME_SOURCE_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace visualmemory {

class VisualMemoryReader;
class PopStore;

struct ObjSrcFrame {
    std::string source_type = "camera";       // "camera" | "display"
    std::string kind;                         // "" | "history" | "checkpoint"
    int64_t     id = 0;
    std::string source_id;                    // camera device id, or the PoP evidence source ("main_display")
    int64_t     capture_utc_ms = 0;
    int         width = 0, height = 0;        // stored JPEG size
    bool        image_available = false;      // DB flag: the JPEG has not been removed by retention
    bool        image_exists = false;         // the file is present right now (camera sources only; display reports the flag)
    std::string sha256;
    // ---- display (Proof-of-Play) provenance; empty / -1 for camera frames ----
    std::string session_id, expected_media_id, reported_media_id, capture_method, provenance;
    int64_t     playback_offset_ms = -1;      // relative to the player-reported play start of the session
    int         time_uncertainty_ms = -1;     // -1: the capture instant has no stated bound
};

class ObjectFrameSource {
public:
    virtual ~ObjectFrameSource() = default;
    virtual std::string type() const = 0;                                   // "camera" | "display"
    // Lazily open the underlying store; false (err set) when it is not available (yet).
    virtual bool ensure_open(std::string& err) = 0;
    virtual bool get(const std::string& kind, int64_t id, ObjSrcFrame& out) = 0;
    // 0 ok, 1 unknown frame, 2 image expired (retention), 3 file missing / unreadable.
    virtual int  read_jpeg(const std::string& kind, int64_t id, std::vector<uint8_t>& out, std::string& err) = 0;
    // Frames captured strictly after `after_utc_ms`, newest first, at most `limit`; `total` = how many frames match (may be larger).
    virtual std::vector<ObjSrcFrame> list_after(int64_t after_utc_ms, int limit, uint64_t* total) = 0;
    // Frames in [start, end) (0 = unbounded), at most `limit`, newest first when `newest_first`.
    virtual std::vector<ObjSrcFrame> list_range(int64_t start_utc_ms, int64_t end_utc_ms, int limit, bool newest_first) = 0;
    // Counts for the dashboard: frames stored / frames whose JPEG is still retained.
    virtual void counts(uint64_t& stored, uint64_t& with_image) = 0;
};

// The set of sources an object search can resolve a hit's parent frame against (either may be null).
struct ObjectSources {
    ObjectFrameSource* camera = nullptr;
    ObjectFrameSource* display = nullptr;
    ObjectFrameSource* get(const std::string& type) const { return type == "display" ? display : camera; }
};

// ---- camera ----
class CameraObjectSource : public ObjectFrameSource {
public:
    explicit CameraObjectSource(VisualMemoryReader& reader) : r_(reader) {}
    std::string type() const override { return "camera"; }
    bool ensure_open(std::string& err) override;
    bool get(const std::string& kind, int64_t id, ObjSrcFrame& out) override;
    int  read_jpeg(const std::string& kind, int64_t id, std::vector<uint8_t>& out, std::string& err) override;
    std::vector<ObjSrcFrame> list_after(int64_t after_utc_ms, int limit, uint64_t* total) override;
    std::vector<ObjSrcFrame> list_range(int64_t start_utc_ms, int64_t end_utc_ms, int limit, bool newest_first) override;
    void counts(uint64_t& stored, uint64_t& with_image) override;
private:
    VisualMemoryReader& r_;
};

// ---- Weston/VOP2 display frames (Proof of Play). Only frames captured through the OS rendered-frame service ("resilience_grpc"). ----
class DisplayObjectSource : public ObjectFrameSource {
public:
    explicit DisplayObjectSource(PopStore& store) : s_(store) {}
    std::string type() const override { return "display"; }
    bool ensure_open(std::string& err) override;
    bool get(const std::string& kind, int64_t id, ObjSrcFrame& out) override;
    int  read_jpeg(const std::string& kind, int64_t id, std::vector<uint8_t>& out, std::string& err) override;
    std::vector<ObjSrcFrame> list_after(int64_t after_utc_ms, int limit, uint64_t* total) override;
    std::vector<ObjSrcFrame> list_range(int64_t start_utc_ms, int64_t end_utc_ms, int limit, bool newest_first) override;
    void counts(uint64_t& stored, uint64_t& with_image) override;
private:
    PopStore& s_;
};

} // namespace visualmemory

#endif // VISUALMEMORY_OBJECT_FRAME_SOURCE_H
