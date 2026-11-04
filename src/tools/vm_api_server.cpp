// SPDX-License-Identifier: MIT
// vm_api_server — thin, read-only HTTP/JSON adapter over libvisualmemory's VisualMemoryReader.
//
// Runs as its OWN process next to Argus (never on the capture thread, never inside the inference
// process). Listens on loopback only; the dashboard-server proxies /api/vm/* to it and enforces
// method/origin/token rules. Frames are addressed by integer id: no filesystem path ever leaves
// this process. Uploaded query images live in memory for the duration of one request.
//
// API contract: docs/VISUAL_MEMORY_DASHBOARD_API.md
#include <3rdparty/nlohmann/json.hpp>

#include "visualmemory/mock_embedding_provider.h"
#include "visualmemory/object_index.h"
#include "visualmemory/reader.h"
#ifdef ARGUS_VISUAL_EMBEDDINGS
#include "rknn_embedding_provider.h"
#endif

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

using json = nlohmann::json;
using namespace visualmemory;
namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxHeader = 16 * 1024;
constexpr size_t kMaxBody = 8u * 1024 * 1024;
constexpr int kMaxConn = 8;
const char* kLabel = "Image/scene similarity (whole-frame MobileNetV2 embedding). Not object detection, not identity matching.";

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

// ---------------------------------------------------------------- configuration
struct Cfg {
    bool        configured = false;           // visual_memory.enabled in the active config
    bool        vector_search = false;
    bool        embedding = false;
    std::string backend = "rknn";
    std::string storage_root;
    std::string model_path;
    int         npu_core = 1;                 // API process default: an idle core, away from the detectors' core 0
    uint64_t    max_storage_bytes = 0;
    int         retention_hours = 0;
    int         embedding_retention_days = 0;
    int         search_timeout_ms = 10000;
    int         idle_unload_s = 120;
    int64_t     sample_interval_ms = 5000;    // visual_memory.sample_interval_ms (library default)
    int         stored_width = 640, stored_height = 360, jpeg_quality = 75;
    bool        objects_enabled = false;      // visual_memory.objects.enabled (D4 manual-ROI object search; off by default)
    uint64_t    objects_max_regions = 10000;
    int         objects_retention_days = 30;
    int         objects_min_roi_px = 16;
    std::string config_path;
    std::string read_error;
};

Cfg load_cfg(const std::string& path) {
    Cfg c; c.config_path = path;
    std::ifstream f(path);
    if (!f) { c.read_error = "config not readable"; return c; }
    try {
        json j = json::parse(f, nullptr, true, /*ignore_comments=*/true);
        const json vm = j.value("visual_memory", json::object());
        c.configured = vm.value("enabled", false);
        c.vector_search = vm.value("vector_search_enabled", false);
        c.storage_root = vm.value("storage_root", std::string("/storage/sd/visual-memory"));
        c.max_storage_bytes = vm.value("max_storage_bytes", static_cast<uint64_t>(0));
        c.retention_hours = vm.value("retention_hours", 0);
        c.sample_interval_ms = vm.value("sample_interval_ms", static_cast<int64_t>(5000));
        c.stored_width = vm.value("stored_width", 640);
        c.stored_height = vm.value("stored_height", 360);
        c.jpeg_quality = vm.value("jpeg_quality", 75);
        const json e = vm.value("embedding", json::object());
        c.embedding = e.value("enabled", false);
        c.backend = e.value("backend", std::string("rknn"));
        c.model_path = e.value("model_path", std::string("model/visual_memory/mobilenet_embedder.rknn"));
        c.embedding_retention_days = e.value("retention_days", 0);
        const json ob = vm.value("objects", json::object());
        c.objects_enabled = ob.value("enabled", false);
        c.objects_max_regions = ob.value("max_regions", static_cast<uint64_t>(10000));
        c.objects_retention_days = ob.value("retention_days", 30);
        c.objects_min_roi_px = std::max(8, std::min(256, ob.value("min_roi_px", 16)));
        const json a = vm.value("api", json::object());
        c.npu_core = a.value("npu_core", 1);
        c.search_timeout_ms = std::max(1000, std::min(60000, a.value("search_timeout_ms", 10000)));
        c.idle_unload_s = std::max(10, a.value("idle_unload_s", 120));
    } catch (const std::exception& ex) {
        c.read_error = std::string("config parse error: ") + ex.what();
        c.configured = false;
    }
    return c;
}

// ---------------------------------------------------------------- HTTP plumbing
struct Request {
    std::string method, path, body;
    std::map<std::string, std::string> query, headers;
};
struct Response {
    int status = 200;
    std::string ctype = "application/json";
    std::string body;
    std::string cache = "no-store";
};

std::string url_decode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() + 0 && std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2])) {
            o += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16)); i += 2;
        } else if (s[i] == '+') o += ' ';
        else o += s[i];
    }
    return o;
}

const char* reason(int s) {
    switch (s) { case 200: return "OK"; case 400: return "Bad Request"; case 404: return "Not Found"; case 405: return "Method Not Allowed";
        case 201: return "Created"; case 410: return "Gone"; case 413: return "Payload Too Large"; case 429: return "Too Many Requests"; case 500: return "Internal Server Error";
        case 503: return "Service Unavailable"; case 504: return "Gateway Timeout"; default: return "Status"; }
}

bool send_all(int fd, const char* p, size_t n) {
    while (n) {
        ssize_t w = ::send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0) return false;
        p += w; n -= static_cast<size_t>(w);
    }
    return true;
}

void send_response(int fd, const Response& r, bool head_only = false) {
    std::ostringstream h;
    h << "HTTP/1.1 " << r.status << " " << reason(r.status) << "\r\nContent-Type: " << r.ctype
      << "\r\nContent-Length: " << r.body.size() << "\r\nCache-Control: " << r.cache
      << "\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n";
    std::string out = h.str();
    // One write for small bodies (avoids the Nagle/delayed-ACK 40 ms stall of header-then-body writes).
    if (!head_only && r.body.size() <= 256 * 1024) { out += r.body; send_all(fd, out.data(), out.size()); return; }
    if (!send_all(fd, out.data(), out.size())) return;
    if (!head_only) send_all(fd, r.body.data(), r.body.size());
}

Response jerr(int status, const std::string& code, const std::string& msg) {
    Response r; r.status = status; r.body = json{{"error", code}, {"message", msg}}.dump(); return r;
}
Response jok(const json& j) { Response r; r.body = j.dump(); return r; }

// Read a request. Returns 0 ok, else an HTTP status to answer with (0 => drop silently).
int read_request(int fd, Request& rq) {
    std::string buf;
    char tmp[4096];
    size_t hdr_end = std::string::npos;
    while (hdr_end == std::string::npos) {
        ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
        if (n <= 0) return -1;
        buf.append(tmp, static_cast<size_t>(n));
        hdr_end = buf.find("\r\n\r\n");
        if (hdr_end == std::string::npos && buf.size() > kMaxHeader) return 431;
    }
    std::istringstream hs(buf.substr(0, hdr_end));
    std::string line;
    std::getline(hs, line);
    std::istringstream rl(line);
    std::string target, ver;
    rl >> rq.method >> target >> ver;
    if (rq.method.empty() || target.empty() || target[0] != '/') return 400;
    const size_t qm = target.find('?');
    rq.path = url_decode(target.substr(0, qm));
    if (qm != std::string::npos) {
        std::istringstream qs(target.substr(qm + 1));
        std::string kv;
        while (std::getline(qs, kv, '&')) {
            const size_t eq = kv.find('=');
            rq.query[url_decode(kv.substr(0, eq))] = eq == std::string::npos ? "" : url_decode(kv.substr(eq + 1));
        }
    }
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t c = line.find(':');
        if (c == std::string::npos) continue;
        std::string k = line.substr(0, c), v = line.substr(c + 1);
        std::transform(k.begin(), k.end(), k.begin(), [](unsigned char ch) { return std::tolower(ch); });
        while (!v.empty() && v.front() == ' ') v.erase(v.begin());
        rq.headers[k] = v;
    }
    std::string rest = buf.substr(hdr_end + 4);
    auto fill = [&](size_t need) {            // ensure rest.size() >= need
        while (rest.size() < need) {
            ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
            if (n <= 0) return false;
            rest.append(tmp, static_cast<size_t>(n));
            if (rest.size() > kMaxBody + 64 * 1024) return false;
        }
        return true;
    };
    auto te = rq.headers.find("transfer-encoding");
    if (te != rq.headers.end() && te->second.find("chunked") != std::string::npos) {
        std::string body;
        for (;;) {
            size_t le;
            while ((le = rest.find("\r\n")) == std::string::npos) { if (!fill(rest.size() + 1)) return 400; }
            const size_t sz = std::strtoul(rest.substr(0, le).c_str(), nullptr, 16);
            rest.erase(0, le + 2);
            if (sz == 0) break;
            if (body.size() + sz > kMaxBody) return 413;
            if (!fill(sz + 2)) return 400;
            body.append(rest, 0, sz); rest.erase(0, sz + 2);
        }
        rq.body = std::move(body);
        return 0;
    }
    auto cl = rq.headers.find("content-length");
    if (cl != rq.headers.end()) {
        const unsigned long long n = std::strtoull(cl->second.c_str(), nullptr, 10);
        if (n > kMaxBody) return 413;
        if (!fill(static_cast<size_t>(n))) return 400;
        rq.body = rest.substr(0, static_cast<size_t>(n));
    }
    return 0;
}

// ---------------------------------------------------------------- application state
struct App {
    Cfg cfg;
    std::string base_dir;                       // extension dir (relative model_path is resolved against it)
    std::string status_file = "/tmp/argus_vm_status.json";
    std::string metrics_addr = "127.0.0.1:9101"; // Argus Prometheus exporter (current FPS)
    bool viewer = false;                        // --viewer: read-only view of a stored dataset, no live Argus writer
    std::string provider_kind = "auto";         // auto (rknn per config) | mock

    std::mutex rd_mu;
    std::unique_ptr<VisualMemoryReader> reader;
    int64_t last_open_try_ms = 0;
    std::string open_err;

    std::mutex obj_mu;
    std::unique_ptr<ObjectIndex> objects;
    int64_t last_obj_try_ms = 0;
    std::string obj_err;
    // Lazily opens <storage_root>/objects.db (own file; visual_memory.db stays read-only here).
    ObjectIndex* get_objects(std::string* why = nullptr) {
        std::lock_guard<std::mutex> lk(obj_mu);
        if (!cfg.configured || !cfg.objects_enabled) { if (why) *why = "object search is not enabled (visual_memory.objects.enabled)"; return nullptr; }
        if (objects && objects->is_open()) return objects.get();
        const int64_t t = steady_ms();
        if (!objects || t - last_obj_try_ms >= 2000) {
            last_obj_try_ms = t;
            ObjectIndexConfig oc; oc.root_directory = cfg.storage_root; oc.min_roi_px = cfg.objects_min_roi_px;
            auto o = std::make_unique<ObjectIndex>(oc);
            std::string e;
            if (o->open(e)) { objects = std::move(o); obj_err.clear(); }
            else { obj_err = e; objects.reset(); }
        }
        if (!objects && why) *why = "object index unavailable: " + obj_err;
        return objects.get();
    }

    std::mutex prov_mu;
    std::shared_ptr<EmbeddingProvider> provider;
    std::string prov_err;
    std::atomic<int64_t> last_search_ms{0};
    std::atomic<bool> searching{false};

    // Returns the reader, (re)opening lazily at most every 2 s.
    VisualMemoryReader* get_reader(std::string* why = nullptr) {
        std::lock_guard<std::mutex> lk(rd_mu);
        if (!cfg.configured) { if (why) *why = "visual memory not configured"; return nullptr; }
        if (reader && reader->is_open()) return reader.get();
        const int64_t t = steady_ms();
        if (t - last_open_try_ms >= 2000 || !reader) {
            last_open_try_ms = t;
            ReaderConfig rc; rc.root_directory = cfg.storage_root;
            auto r = std::make_unique<VisualMemoryReader>(rc);
            std::string e;
            if (r->open(e)) { reader = std::move(r); open_err.clear(); }
            else { open_err = e; reader.reset(); }
        }
        if (!reader && why) *why = "store unavailable: " + open_err;
        return reader.get();
    }

    // Lazy model load (first upload search). The RKNN context is released after idle_unload_s.
    std::shared_ptr<EmbeddingProvider> get_provider(std::string& why) {
        std::lock_guard<std::mutex> lk(prov_mu);
        if (provider && provider->ready()) return provider;
        provider.reset();
        if (!cfg.vector_search || !cfg.embedding) { why = "embeddings not enabled in the active configuration"; return nullptr; }
        if (provider_kind.rfind("mock", 0) == 0) {       // dev/tests only: "mock" or "mock:<dim>"
            const size_t c = provider_kind.find(':');
            provider = std::make_shared<MockEmbeddingProvider>(c == std::string::npos ? 64 : std::atoi(provider_kind.c_str() + c + 1));
            return provider;
        }
#ifdef ARGUS_VISUAL_EMBEDDINGS
        if (cfg.backend != "rknn") { why = "unsupported embedding backend '" + cfg.backend + "'"; return nullptr; }
        RknnEmbeddingProvider::Options ro;
        ro.model_path = cfg.model_path.rfind('/', 0) == 0 || base_dir.empty() ? cfg.model_path : base_dir + "/" + cfg.model_path;
        ro.npu_core = cfg.npu_core;
        auto p = std::make_shared<RknnEmbeddingProvider>(ro);
        if (!p->init()) { prov_err = p->last_error(); why = "embedding model failed to load: " + prov_err; return nullptr; }
        provider = p;
        return provider;
#else
        why = "this build has no RKNN backend";
        return nullptr;
#endif
    }
    bool provider_loaded() { std::lock_guard<std::mutex> lk(prov_mu); return provider && provider->ready(); }
    void maybe_unload() {
        std::lock_guard<std::mutex> lk(prov_mu);
        if (provider && !searching.load() && steady_ms() - last_search_ms.load() > cfg.idle_unload_s * 1000LL) {
            provider->shutdown(); provider.reset();
        }
    }
};

json model_json(const EmbeddingSpec& s) {
    return {{"name", s.model_name}, {"version", s.model_version}, {"preprocessing", s.preprocessing},
            {"dimension", s.dimension}, {"sha256", s.model_sha256}, {"dtype", s.dtype}, {"metric", s.metric}};
}

json frame_json(const FrameRecord& r) {
    const FrameInfo& f = r.frame;
    return {{"id", f.id}, {"capture_utc_ms", f.metadata.capture_utc_ms}, {"source_id", f.metadata.source_id},
            {"width", f.width}, {"height", f.height}, {"jpeg_bytes", f.file_size_bytes},
            {"image_available", f.image_available}, {"image_exists", r.image_exists}, {"has_embedding", r.has_embedding}};
}

// ---------------------------------------------------------------- handlers
json read_status_file(const App& a, bool& have, int64_t& age) {
    have = false; age = -1;
    std::ifstream f(a.status_file);
    if (!f) return json();
    try {
        json j = json::parse(f);
        const int64_t w = j.value("written_utc_ms", static_cast<int64_t>(0));
        if (w > 0) { have = true; age = now_ms() - w; }
        return j;
    } catch (...) { return json(); }
}

// Current capture/processing FPS comes from Argus's existing Prometheus exporter (argus_fps_current), read over
// loopback with a short timeout and cached for 2 s. No new counters are added to Argus; unavailable => null.
struct CaptureStats { bool ok = false; double fps = 0, dropped = 0, width = 0, height = 0; int64_t at_ms = 0; };
CaptureStats read_capture_stats(const std::string& host_port) {
    static std::mutex mu; static CaptureStats cache; static int64_t tried_ms = 0;
    std::lock_guard<std::mutex> lk(mu);
    const int64_t t = now_ms();
    if (t - tried_ms < 2000) return cache;
    tried_ms = t;
    CaptureStats cs;
    const auto colon = host_port.rfind(':');
    const int port = colon == std::string::npos ? 9101 : std::atoi(host_port.c_str() + colon + 1);
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd >= 0) {
        sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_port = htons(static_cast<uint16_t>(port));
        ::inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
        bool ok = ::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0 || errno == EINPROGRESS;
        pollfd pf{fd, POLLOUT, 0};
        if (ok && ::poll(&pf, 1, 300) == 1 && (pf.revents & POLLOUT)) {
            static const char req[] = "GET /metrics HTTP/1.0\r\nHost: localhost\r\nConnection: close\r\n\r\n";
            if (::send(fd, req, sizeof req - 1, MSG_NOSIGNAL) > 0) {
                std::string buf; char tmp[4096];
                const int64_t until = now_ms() + 500;
                while (buf.size() < (256u << 10) && now_ms() < until) {
                    pollfd rp{fd, POLLIN, 0};
                    if (::poll(&rp, 1, 100) <= 0) continue;
                    const ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
                    if (n <= 0) break;
                    buf.append(tmp, static_cast<size_t>(n));
                }
                auto val = [&](const char* name, double& out) {
                    const size_t p = buf.find(std::string("\n") + name);
                    if (p == std::string::npos) return false;
                    const size_t e = buf.find('\n', p + 1);
                    const size_t sp = buf.rfind(' ', e == std::string::npos ? buf.size() : e);
                    if (sp == std::string::npos || sp < p) return false;
                    out = std::atof(buf.c_str() + sp + 1); return true;
                };
                cs.ok = val("argus_fps_current", cs.fps);
                val("argus_dropped_frames_total", cs.dropped);
                val("argus_frame_width", cs.width); val("argus_frame_height", cs.height);
                cs.at_ms = now_ms();
            }
        }
        ::close(fd);
    }
    cache = cs;
    return cache;
}

Response handle_status(App& a) {
    json out; json errors = json::array();
    out["server_time_utc_ms"] = now_ms();
    out["configured"] = a.cfg.configured;
    out["config_path"] = a.cfg.config_path;
    out["vector_search_configured"] = a.cfg.configured && a.cfg.vector_search;
    out["embedding_configured"] = a.cfg.configured && a.cfg.vector_search && a.cfg.embedding;
    if (!a.cfg.read_error.empty()) errors.push_back(a.cfg.read_error);
    if (!a.cfg.configured) {
        for (const char* k : {"runtime", "model", "store", "search", "sampling", "capture"}) out[k] = nullptr;
        out["objects"] = {{"enabled", false}};
        out["errors"] = errors;
        return jok(out);
    }
    // runtime snapshot written by Argus
    bool have; int64_t age;
    json snap = read_status_file(a, have, age);
    json rt = json::object();
    const bool stale = !have || age > 20000;
    rt["running"] = have && !stale;
    rt["snapshot_age_ms"] = have ? json(age) : json(nullptr);
    rt["stale"] = stale;
    rt["stage1"] = have && snap.contains("stage1") ? snap["stage1"] : json(nullptr);
    rt["stage2"] = have && snap.contains("stage2") ? snap["stage2"] : json(nullptr);
    if (a.viewer) {}   // dataset viewer: there is intentionally no live Argus writer behind this store
    else if (!have) errors.push_back("no runtime snapshot from Argus (Argus not running, or built without Visual Memory)");
    else if (stale) errors.push_back("runtime snapshot stale (" + std::to_string(age / 1000) + " s): Argus may be stopped");
    out["runtime"] = a.viewer ? json(nullptr) : rt;
    out["viewer"] = a.viewer;
    // Embeddings configured, Argus alive, but Stage 2 not running there (model failed to load, backend
    // unsupported, ...): say so instead of showing a silently idle pipeline.
    if (a.cfg.vector_search && a.cfg.embedding && have && !stale && !(snap.contains("stage2") && snap["stage2"].is_object()))
        errors.push_back("embeddings are configured but Stage 2 is not running in Argus (model failed to load or backend unavailable; see the Argus log)");
    else if (have && !stale && snap.contains("stage2") && snap["stage2"].is_object() &&
             snap["stage2"].value("status", std::string("active")) != "active")
        errors.push_back("Stage 2 inactive: " + snap["stage2"].value("status", std::string("unknown")));

    json model = nullptr;
    if (have && snap.contains("model") && snap["model"].is_object()) model = snap["model"];

    // store
    std::string why;
    VisualMemoryReader* rd = a.get_reader(&why);
    json store = {{"available", false}};
    json search = {{"available", false}, {"reason", ""}, {"model_loaded", a.provider_loaded()}, {"busy", a.searching.load()}};
    if (!rd) {
        errors.push_back(why);
        search["reason"] = why;
    } else {
        const StoreSummary s = rd->summary();
        store = {{"available", s.open}, {"schema_version", s.schema_version},
                 {"vec_backend", s.vec_version.empty() ? std::string("blob-cosine-fallback") : "sqlite-vec " + s.vec_version},
                 {"frames", s.frames}, {"frames_with_image", s.frames_with_image},
                 {"embeddings_ok", s.embeddings_ok}, {"embeddings_failed", s.embeddings_failed},
                 {"jpeg_bytes", s.jpeg_bytes}, {"vector_bytes", s.vector_bytes}, {"db_bytes", s.db_bytes},
                 {"quota_bytes", a.cfg.max_storage_bytes}, {"retention_hours", a.cfg.retention_hours},
                 {"embedding_retention_days", a.cfg.embedding_retention_days},
                 {"oldest_utc_ms", s.oldest_capture_utc_ms}, {"newest_utc_ms", s.newest_capture_utc_ms},
                 {"sources", s.sources}};
        std::error_code ec;
        const auto sp = fs::space(a.cfg.storage_root, ec);
        store["disk_free_bytes"] = ec ? json(nullptr) : json(static_cast<uint64_t>(sp.available));
        if (model.is_null() && !s.models.empty()) model = model_json(s.models.front().spec);
        if (s.frames_with_image > s.embeddings_ok + s.embeddings_failed + 12)
            errors.push_back(std::to_string(s.frames_with_image - s.embeddings_ok - s.embeddings_failed) +
                             " stored frames have no embedding yet (embedding paused, backlog, or Stage 2 inactive)");
        if (!a.cfg.vector_search || !a.cfg.embedding) search["reason"] = "embeddings are not enabled in the active configuration";
        else if (s.models.empty() || s.embeddings_ok == 0) search["reason"] = "no embeddings stored yet";
        else { search["available"] = true; }
    }
    out["sampling"] = {{"interval_ms", a.cfg.sample_interval_ms}, {"stored_width", a.cfg.stored_width},
                       {"stored_height", a.cfg.stored_height}, {"jpeg_quality", a.cfg.jpeg_quality}};
    {
        const CaptureStats cs = read_capture_stats(a.metrics_addr);
        if (cs.ok) out["capture"] = {{"fps", cs.fps}, {"dropped_frames", cs.dropped}, {"width", cs.width}, {"height", cs.height},
                                     {"source", "argus metrics"}};
        else out["capture"] = nullptr;
    }
    {
        json ob = {{"enabled", a.cfg.configured && a.cfg.objects_enabled}};
        if (a.cfg.configured && a.cfg.objects_enabled) {
            std::string w; ObjectIndex* oi = a.get_objects(&w);
            if (oi) { const ObjectIndexSummary os = oi->summary();
                ob["available"] = true; ob["regions"] = os.regions; ob["vector_bytes"] = os.vector_bytes; ob["db_bytes"] = os.db_bytes;
                ob["schema_version"] = os.schema_version; ob["max_regions"] = a.cfg.objects_max_regions;
                ob["oldest_utc_ms"] = os.oldest_capture_utc_ms; ob["newest_utc_ms"] = os.newest_capture_utc_ms;
                ob["models"] = json::array(); for (auto& m : os.models) ob["models"].push_back(model_json(m)); }
            else { ob["available"] = false; ob["reason"] = w; }
        }
        out["objects"] = ob;
    }
    out["store"] = store;
    out["model"] = model;
    out["search"] = search;
    out["errors"] = errors;
    return jok(out);
}

bool parse_i64(const std::string& s, int64_t& v) {
    if (s.empty()) return false;
    char* e = nullptr; errno = 0;
    const long long x = std::strtoll(s.c_str(), &e, 10);
    if (errno || *e) return false;
    v = x; return true;
}

Response handle_frames(App& a, const Request& rq) {
    std::string why;
    VisualMemoryReader* rd = a.get_reader(&why);
    if (!rd) return jerr(503, "store_unavailable", why);
    FrameFilter f; int64_t lim = 24, off = 0, v;
    if (rq.query.count("start") && parse_i64(rq.query.at("start"), v)) f.start_utc_ms = v;
    if (rq.query.count("end") && parse_i64(rq.query.at("end"), v)) f.end_utc_ms = v;
    if (rq.query.count("source")) f.source_id = rq.query.at("source");
    if (rq.query.count("limit")) { if (!parse_i64(rq.query.at("limit"), lim) || lim < 1 || lim > 100) return jerr(400, "invalid_argument", "limit must be 1..100"); }
    if (rq.query.count("offset")) { if (!parse_i64(rq.query.at("offset"), off) || off < 0) return jerr(400, "invalid_argument", "offset must be >= 0"); }
    const bool desc = !rq.query.count("order") || rq.query.at("order") != "asc";
    uint64_t total = 0;
    auto page = rd->list_frames(f, static_cast<int>(lim), static_cast<int>(off), desc, &total);
    json arr = json::array();
    for (const auto& r : page) arr.push_back(frame_json(r));
    return jok({{"total", total}, {"limit", lim}, {"offset", off}, {"frames", arr}});
}

Response handle_frame_detail(App& a, int64_t id) {
    std::string why;
    VisualMemoryReader* rd = a.get_reader(&why);
    if (!rd) return jerr(503, "store_unavailable", why);
    FrameDetail d;
    if (!rd->get_frame(id, d)) return jerr(404, "not_found", "no such frame");
    const FrameInfo& f = d.record.frame;
    json emb = json::array();
    for (const auto& e : d.embeddings)
        emb.push_back({{"model_name", e.model_name}, {"model_version", e.model_version}, {"preprocessing", e.preprocessing},
                       {"dimension", e.dimension}, {"status", e.ok ? "ok" : "failed"}, {"created_utc_ms", e.created_utc_ms},
                       {"error", e.error}});
    json ret = {{"jpeg_expires_utc_ms", a.cfg.retention_hours > 0 ? json(f.metadata.capture_utc_ms + a.cfg.retention_hours * 3600000LL) : json(nullptr)},
                {"embedding_expires_utc_ms", a.cfg.embedding_retention_days > 0 ? json(f.metadata.capture_utc_ms + a.cfg.embedding_retention_days * 86400000LL) : json(nullptr)}};
    return jok({{"id", f.id}, {"capture_utc_ms", f.metadata.capture_utc_ms}, {"created_utc_ms", f.created_utc_ms},
                {"source_id", f.metadata.source_id}, {"source_type", f.metadata.source_type}, {"device_id", f.metadata.device_id},
                {"width", f.width}, {"height", f.height}, {"jpeg_bytes", f.file_size_bytes}, {"sha256", f.sha256_hex},
                {"sequence", f.metadata.source_sequence}, {"image_available", f.image_available},
                {"image_exists", d.record.image_exists}, {"embeddings", emb}, {"retention", ret}});
}

Response handle_image(App& a, int64_t id, bool thumb, const Request& rq) {
    std::string why;
    VisualMemoryReader* rd = a.get_reader(&why);
    if (!rd) return jerr(503, "store_unavailable", why);
    std::vector<uint8_t> bytes; std::string err; int rc;
    if (thumb) {
        int64_t w = 240;
        if (rq.query.count("w") && (!parse_i64(rq.query.at("w"), w) || w < 64 || w > 640)) return jerr(400, "invalid_argument", "w must be 64..640");
        rc = rd->read_thumbnail(id, static_cast<int>(w), bytes, err);
    } else rc = rd->read_jpeg(id, bytes, err);
    if (rc == 1) return jerr(404, "not_found", "no such frame");
    if (rc == 2) return jerr(410, "image_expired", "JPEG expired; metadata and embedding are retained");
    if (rc == 3) return jerr(404, "image_missing", "JPEG file is missing or unreadable");
    Response r; r.ctype = "image/jpeg"; r.cache = "private, max-age=300";
    r.body.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return r;
}

const char* code_name(SearchStatus::Code c) {
    switch (c) {
        case SearchStatus::Code::Ok: return "ok"; case SearchStatus::Code::NotEnabled: return "not_enabled";
        case SearchStatus::Code::DimensionMismatch: return "dimension_mismatch"; case SearchStatus::Code::InvalidVector: return "invalid_vector";
        case SearchStatus::Code::ModelMismatch: return "model_mismatch"; case SearchStatus::Code::ProviderFailed: return "provider_failed";
        case SearchStatus::Code::InvalidArgument: return "invalid_argument"; case SearchStatus::Code::DatabaseError: return "database_error";
        case SearchStatus::Code::Cancelled: return "cancelled";
    }
    return "unknown";
}

Response handle_search(App& a, const Request& rq, int fd, bool by_frame) {
    std::string why;
    if (!a.cfg.configured) return jerr(503, "not_enabled", "visual memory is not configured");
    if (!a.cfg.vector_search || !a.cfg.embedding) return jerr(503, "not_enabled", "embeddings/vector search are not enabled in the active configuration");
    VisualMemoryReader* rd = a.get_reader(&why);
    if (!rd) return jerr(503, "store_unavailable", why);

    SearchOptions o; int64_t v;
    o.top_k = 5;
    if (rq.query.count("top_k")) { if (!parse_i64(rq.query.at("top_k"), v) || v < 1 || v > 50) return jerr(400, "invalid_argument", "top_k must be 1..50"); o.top_k = static_cast<int>(v); }
    if (rq.query.count("start") && parse_i64(rq.query.at("start"), v)) o.start_utc_ms = v;
    if (rq.query.count("end") && parse_i64(rq.query.at("end"), v)) o.end_utc_ms = v;
    if (rq.query.count("source")) o.source_id = rq.query.at("source");
    if (rq.query.count("min_similarity") && !rq.query.at("min_similarity").empty()) {
        char* e = nullptr; const float m = std::strtof(rq.query.at("min_similarity").c_str(), &e);
        if (*e || !std::isfinite(m) || m < -1.f || m > 1.f) return jerr(400, "invalid_argument", "min_similarity must be in [-1,1]");
        o.has_min_similarity = true; o.min_similarity = m;
    }
    o.require_image = rq.query.count("require_image") && rq.query.at("require_image") == "1";

    int64_t frame_id = 0;
    if (by_frame) {
        if (!rq.query.count("frame_id") || !parse_i64(rq.query.at("frame_id"), frame_id) || frame_id < 1) return jerr(400, "invalid_argument", "frame_id required");
    } else {
        if (rq.body.size() < 4 || static_cast<uint8_t>(rq.body[0]) != 0xFF || static_cast<uint8_t>(rq.body[1]) != 0xD8)
            return jerr(400, "invalid_argument", "body must be a JPEG image");
    }

    bool expected = false;
    if (!a.searching.compare_exchange_strong(expected, true)) return jerr(429, "busy", "another search is running");
    struct Guard { App& a; ~Guard() { a.last_search_ms = steady_ms(); a.searching = false; } } guard{a};

    // Provider: needed for uploads; for frame search it is optional (stored vector is used when present).
    std::shared_ptr<EmbeddingProvider> prov = nullptr;
    std::string pwhy;
    if (by_frame) {
        // Frame search uses the stored vector: do not load the NPU model for it. Use a provider only if
        // one is already resident (it then also pins the result space), and load lazily as a fallback.
        { std::lock_guard<std::mutex> lk(a.prov_mu); if (a.provider && a.provider->ready()) prov = a.provider; }
    } else {
        prov = a.get_provider(pwhy);
        if (!prov) return jerr(503, "provider_failed", pwhy);
    }
    rd->set_embedding_provider(prov);          // null clears a stale (unloaded) provider

    // Watchdog: deadline and client disconnect both interrupt the SQLite scan. A self-pipe wakes it the
    // instant the search ends, so a fast search is not delayed by the poll interval.
    int wake[2] = {-1, -1};
    if (::pipe(wake) != 0) return jerr(500, "internal", "pipe failed");
    std::atomic<bool> done{false}; std::atomic<int> why_cancel{0};   // 1 deadline, 2 client gone
    std::thread wd([&] {
        const int64_t t0 = steady_ms();
        while (!done.load()) {
            pollfd p[2] = {{fd, POLLRDHUP, 0}, {wake[0], POLLIN, 0}};
            const int n = ::poll(p, 2, 100);
            if (n > 0 && (p[1].revents & POLLIN)) return;
            if (n > 0 && (p[0].revents & (POLLRDHUP | POLLHUP | POLLERR))) { why_cancel = 2; rd->cancel_search(); return; }
            if (steady_ms() - t0 > a.cfg.search_timeout_ms) { why_cancel = 1; rd->cancel_search(); return; }
        }
    });

    SearchStatus st; std::vector<SearchResult> res; bool stored = false;
    if (by_frame) {
        res = rd->search_by_frame_id(frame_id, o, &st, &stored);
        if (st.code == SearchStatus::Code::NotEnabled && !prov) {        // no stored vector: need the model
            prov = a.get_provider(pwhy);
            if (prov) { rd->set_embedding_provider(prov); res = rd->search_by_frame_id(frame_id, o, &st, &stored); }
        }
    }
    else {
        std::vector<uint8_t> jpeg(rq.body.begin(), rq.body.end());
        res = rd->search_by_jpeg(jpeg, o, &st);
    }
    done = true; { const char c = 1; (void)!::write(wake[1], &c, 1); }
    wd.join();
    ::close(wake[0]); ::close(wake[1]);

    std::string code = code_name(st.code);
    int http = 200;
    if (st.code == SearchStatus::Code::Cancelled) { code = why_cancel == 1 ? "timeout" : "cancelled"; http = why_cancel == 1 ? 504 : 200; }
    else if (st.code == SearchStatus::Code::NotEnabled || st.code == SearchStatus::Code::ProviderFailed) http = 503;
    else if (st.code == SearchStatus::Code::InvalidArgument || st.code == SearchStatus::Code::DimensionMismatch ||
             st.code == SearchStatus::Code::InvalidVector || st.code == SearchStatus::Code::ModelMismatch) http = 400;
    else if (st.code == SearchStatus::Code::DatabaseError) http = 500;

    json results = json::array(); int rank = 0;
    for (const auto& r : res) {
        FrameDetail fd; int w = 0, h = 0;
        if (rd->get_frame(r.frame_id, fd)) { w = fd.record.frame.width; h = fd.record.frame.height; }
        results.push_back({{"rank", ++rank}, {"frame_id", r.frame_id}, {"similarity", r.similarity}, {"distance", r.distance},
                           {"capture_utc_ms", r.metadata.capture_utc_ms}, {"source_id", r.metadata.source_id},
                           {"width", w}, {"height", h},
                           {"image_available", r.image_available}, {"image_exists", r.image_exists}});
    }
    json model = nullptr;
    if (prov) model = model_json(prov->spec());
    else { const StoreSummary ss = rd->summary(); if (ss.models.size() == 1) model = model_json(ss.models[0].spec); }
    json out = {{"label", kLabel},
                {"query", {{"type", by_frame ? "frame" : "upload"}, {"frame_id", by_frame ? json(frame_id) : json(nullptr)},
                           {"embedded_with", by_frame && stored ? std::string("stored vector") : (prov ? prov->spec().model_name : std::string("n/a"))}}},
                {"model", model},
                {"status", {{"code", code}, {"message", st.code == SearchStatus::Code::Ok ? std::string() : st.message},
                            {"elapsed_ms", st.elapsed_ms}, {"candidates_scanned", st.candidates_scanned}}},
                {"results", results}};
    Response r = jok(out); r.status = http;
    return r;
}


// ---------------------------------------------------------------- D4: object regions (manual ROI)
const char* kObjLabel = "Object-region similarity (MobileNetV2 embedding of a manually selected crop). Scores are cosine similarities, not identification probabilities.";

json region_json(const ObjectRegion& r) {
    return {{"region_id", r.region_id}, {"frame_id", r.frame_id}, {"source_id", r.source_id}, {"capture_utc_ms", r.capture_utc_ms},
            {"bbox", {{"x", r.roi.x}, {"y", r.roi.y}, {"w", r.roi.w}, {"h", r.roi.h}}},
            {"image_width", r.image_width}, {"image_height", r.image_height}, {"label", r.label}, {"created_utc_ms", r.created_utc_ms},
            {"crop_stored", r.has_crop_file}};
}
json hit_json(const ObjectHit& h, int rank) {
    json j = region_json(h.region);
    j["rank"] = rank; j["similarity"] = h.similarity; j["distance"] = h.distance;
    j["image_available"] = h.image_available; j["image_exists"] = h.image_exists;
    return j;
}

bool body_json(const Request& rq, json& j, std::string& why) {
    if (rq.body.empty() || rq.body.size() > 16 * 1024) { why = "JSON body required (max 16 KiB)"; return false; }
    j = json::parse(rq.body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { why = "body must be a JSON object"; return false; }
    return true;
}
bool get_int(const json& j, const char* k, int64_t& v) {
    if (!j.contains(k)) return false;
    const json& x = j[k];
    if (x.is_number_integer()) { v = x.get<int64_t>(); return true; }
    if (x.is_number_float()) { const double d = x.get<double>(); if (std::isfinite(d) && std::floor(d) == d && std::fabs(d) < 1e9) { v = static_cast<int64_t>(d); return true; } }
    return false;
}
bool get_roi(const json& j, Roi& r, std::string& why) {
    int64_t x, y, w, h;
    if (!get_int(j, "x", x) || !get_int(j, "y", y) || !get_int(j, "w", w) || !get_int(j, "h", h)) { why = "x, y, w, h (integers) required"; return false; }
    if (std::llabs(x) > 100000 || std::llabs(y) > 100000 || std::llabs(w) > 100000 || std::llabs(h) > 100000) { why = "rectangle out of range"; return false; }
    r = {static_cast<int>(x), static_cast<int>(y), static_cast<int>(w), static_cast<int>(h)};
    return true;
}

// Runs fn() with the deadline + client-disconnect watchdog used by the full-frame search.
template <class F> int run_watched(App& a, int fd, const std::function<void()>& cancel, F&& fn) {
    int wake[2] = {-1, -1};
    if (::pipe(wake) != 0) { fn(); return 0; }
    std::atomic<bool> done{false}; std::atomic<int> why{0};
    std::thread wd([&] {
        const int64_t t0 = steady_ms();
        while (!done.load()) {
            pollfd p[2] = {{fd, POLLRDHUP, 0}, {wake[0], POLLIN, 0}};
            const int n = ::poll(p, 2, 100);
            if (n > 0 && (p[1].revents & POLLIN)) return;
            if (n > 0 && (p[0].revents & (POLLRDHUP | POLLHUP | POLLERR))) { why = 2; cancel(); return; }
            if (steady_ms() - t0 > a.cfg.search_timeout_ms) { why = 1; cancel(); return; }
        }
    });
    fn();
    done = true; { const char c = 1; (void)!::write(wake[1], &c, 1); }
    wd.join(); ::close(wake[0]); ::close(wake[1]);
    return why.load();
}

struct NpuGuard {
    App& a; bool held = false;
    explicit NpuGuard(App& app) : a(app) { bool e = false; held = a.searching.compare_exchange_strong(e, true); }
    ~NpuGuard() { if (held) { a.last_search_ms = steady_ms(); a.searching = false; } }
};

Response objects_gate(App& a, ObjectIndex*& oi, VisualMemoryReader*& rd) {
    std::string why;
    if (!a.cfg.configured) return jerr(503, "not_enabled", "visual memory is not configured");
    if (!a.cfg.objects_enabled) return jerr(503, "not_enabled", "object search is not enabled (visual_memory.objects.enabled)");
    rd = a.get_reader(&why); if (!rd) return jerr(503, "store_unavailable", why);
    oi = a.get_objects(&why); if (!oi) return jerr(503, "store_unavailable", why);
    return Response{200, "", "", ""};
}

Response handle_frame_crop(App& a, int64_t id, const Request& rq) {
    std::string why; VisualMemoryReader* rd = a.get_reader(&why);
    if (!rd) return jerr(503, "store_unavailable", why);
    int64_t x, y, w, h, mx = 240;
    auto q = [&](const char* k, int64_t& v) { return rq.query.count(k) && parse_i64(rq.query.at(k), v); };
    if (!q("x", x) || !q("y", y) || !q("w", w) || !q("h", h)) return jerr(400, "invalid_argument", "x, y, w, h required");
    if (rq.query.count("max") && (!parse_i64(rq.query.at("max"), mx) || mx < 32 || mx > 640)) return jerr(400, "invalid_argument", "max must be 32..640");
    ObjectIndex scratch({a.cfg.storage_root});              // crop generation needs no database
    std::vector<uint8_t> out; std::string err;
    const int rc = scratch.crop_jpeg(*rd, id, {static_cast<int>(std::max<int64_t>(-1, std::min<int64_t>(x, 100000))), static_cast<int>(std::max<int64_t>(-1, std::min<int64_t>(y, 100000))),
                                              static_cast<int>(std::min<int64_t>(w, 100000)), static_cast<int>(std::min<int64_t>(h, 100000))}, static_cast<int>(mx), 88, out, err);
    if (rc == 1) return jerr(404, "not_found", "no such frame");
    if (rc == 2) return jerr(410, "image_expired", "JPEG expired");
    if (rc == 3) return jerr(404, "image_missing", "JPEG file is missing or unreadable");
    if (rc == 4) return jerr(400, "invalid_roi", err);
    if (rc != 0) return jerr(500, "internal", "crop failed");
    Response r; r.ctype = "image/jpeg"; r.cache = "private, max-age=60";
    r.body.assign(reinterpret_cast<const char*>(out.data()), out.size());
    return r;
}

Response handle_objects_list(App& a, const Request& rq) {
    ObjectIndex* oi = nullptr; VisualMemoryReader* rd = nullptr;
    Response g = objects_gate(a, oi, rd); if (!oi) return g;
    ObjectFilter f; int64_t v, lim = 24, off = 0;
    if (rq.query.count("start") && parse_i64(rq.query.at("start"), v)) f.start_utc_ms = v;
    if (rq.query.count("end") && parse_i64(rq.query.at("end"), v)) f.end_utc_ms = v;
    if (rq.query.count("source")) f.source_id = rq.query.at("source");
    if (rq.query.count("label")) f.label = rq.query.at("label");
    if (rq.query.count("frame_id") && parse_i64(rq.query.at("frame_id"), v) && v > 0) f.frame_id = v;
    if (rq.query.count("limit")) { if (!parse_i64(rq.query.at("limit"), lim) || lim < 1 || lim > 100) return jerr(400, "invalid_argument", "limit must be 1..100"); }
    if (rq.query.count("offset")) { if (!parse_i64(rq.query.at("offset"), off) || off < 0) return jerr(400, "invalid_argument", "offset must be >= 0"); }
    uint64_t total = 0;
    const auto rows = oi->list_regions(f, static_cast<int>(lim), static_cast<int>(off), true, &total);
    json arr = json::array(); for (auto& r : rows) arr.push_back(region_json(r));
    return jok({{"label", kObjLabel}, {"total", total}, {"limit", lim}, {"offset", off}, {"objects", arr}});
}

Response handle_object_detail(App& a, int64_t id) {
    ObjectIndex* oi = nullptr; VisualMemoryReader* rd = nullptr;
    Response g = objects_gate(a, oi, rd); if (!oi) return g;
    ObjectRegion r; if (!oi->get_region(id, r)) return jerr(404, "not_found", "no such object region");
    json j = region_json(r);
    FrameDetail fd;
    if (rd->get_frame(r.frame_id, fd)) { j["image_available"] = fd.record.frame.image_available; j["image_exists"] = fd.record.image_exists; }
    return jok(j);
}

Response handle_object_image(App& a, int64_t id, const Request& rq) {
    ObjectIndex* oi = nullptr; VisualMemoryReader* rd = nullptr;
    Response g = objects_gate(a, oi, rd); if (!oi) return g;
    int64_t mx = 240;
    if (rq.query.count("max") && (!parse_i64(rq.query.at("max"), mx) || mx < 32 || mx > 640)) return jerr(400, "invalid_argument", "max must be 32..640");
    std::vector<uint8_t> out; std::string err;
    const int rc = oi->region_crop_jpeg(*rd, id, static_cast<int>(mx), 88, out, err);
    if (rc == 1) return jerr(404, "not_found", "no such object region or frame");
    if (rc == 2) return jerr(410, "image_expired", "source frame JPEG expired; the embedding and bounding box are retained");
    if (rc == 3) return jerr(404, "image_missing", "source frame JPEG is missing");
    if (rc != 0) return jerr(500, "internal", "crop failed");
    Response r; r.ctype = "image/jpeg"; r.cache = "private, max-age=300";
    r.body.assign(reinterpret_cast<const char*>(out.data()), out.size());
    return r;
}

Response handle_object_index(App& a, const Request& rq, int fd) {
    ObjectIndex* oi = nullptr; VisualMemoryReader* rd = nullptr;
    Response g = objects_gate(a, oi, rd); if (!oi) return g;
    if (!a.cfg.embedding) return jerr(503, "not_enabled", "embeddings are not enabled in the active configuration");
    json j; std::string why;
    if (!body_json(rq, j, why)) return jerr(400, "invalid_argument", why);
    int64_t frame_id = 0; Roi roi;
    if (!get_int(j, "frame_id", frame_id) || frame_id < 1) return jerr(400, "invalid_argument", "frame_id required");
    if (!get_roi(j, roi, why)) return jerr(400, "invalid_argument", why);
    std::string label = j.contains("label") && j["label"].is_string() ? j["label"].get<std::string>() : std::string();
    NpuGuard ng(a);
    if (!ng.held) return jerr(429, "busy", "another search or indexing operation is running");
    auto prov = a.get_provider(why); if (!prov) return jerr(503, "provider_failed", why);
    ObjectIndexResult res;
    run_watched(a, fd, [] {}, [&] { res = oi->index_region(*rd, *prov, frame_id, roi, label); });
    using C = ObjectIndexResult::Code;
    int http = 200; std::string code = "ok";
    switch (res.code) {
        case C::Indexed: http = 201; code = "indexed"; break;
        case C::Duplicate: code = "duplicate"; break;
        case C::FrameNotFound: http = 404; code = "not_found"; break;
        case C::ImageUnavailable: http = 410; code = "image_unavailable"; break;
        case C::InvalidRoi: http = 400; code = "invalid_roi"; break;
        case C::InvalidArgument: http = 400; code = "invalid_argument"; break;
        case C::DecodeFailed: http = 500; code = "decode_failed"; break;
        case C::ProviderFailed: http = 503; code = "provider_failed"; break;
        case C::InvalidVector: http = 500; code = "invalid_vector"; break;
        default: http = 500; code = "database_error"; break;
    }
    if (http >= 400) { Response r = jerr(http, code, res.message); return r; }
    ObjectRegion reg; oi->get_region(res.region_id, reg);
    // independent retention for the object index (bounded work: only when a region was added)
    uint64_t pruned = 0;
    if (res.code == C::Indexed) {
        const int64_t cutoff = a.cfg.objects_retention_days > 0 ? now_ms() - static_cast<int64_t>(a.cfg.objects_retention_days) * 86400000LL : 0;
        pruned = oi->prune(cutoff, a.cfg.objects_max_regions);
    }
    json out = {{"label", kObjLabel}, {"status", code}, {"region", region_json(reg)}, {"elapsed_ms", res.elapsed_ms}, {"pruned", pruned},
                {"model", model_json(prov->spec())}};
    Response r = jok(out); r.status = http; return r;
}

Response handle_object_search(App& a, const Request& rq, int fd) {
    ObjectIndex* oi = nullptr; VisualMemoryReader* rd = nullptr;
    Response g = objects_gate(a, oi, rd); if (!oi) return g;
    if (!a.cfg.embedding) return jerr(503, "not_enabled", "embeddings are not enabled in the active configuration");
    json j; std::string why;
    if (!body_json(rq, j, why)) return jerr(400, "invalid_argument", why);
    ObjectSearchOptions o; int64_t v;
    o.top_k = 5;
    if (get_int(j, "top_k", v)) { if (v < 1 || v > 50) return jerr(400, "invalid_argument", "top_k must be 1..50"); o.top_k = static_cast<int>(v); }
    if (get_int(j, "start", v)) o.start_utc_ms = v;
    if (get_int(j, "end", v)) o.end_utc_ms = v;
    if (j.contains("source") && j["source"].is_string()) o.source_id = j["source"].get<std::string>();
    if (j.contains("label") && j["label"].is_string()) o.label = j["label"].get<std::string>();
    if (j.contains("min_similarity") && j["min_similarity"].is_number()) {
        const double m = j["min_similarity"].get<double>();
        if (!std::isfinite(m) || m < -1 || m > 1) return jerr(400, "invalid_argument", "min_similarity must be in [-1,1]");
        o.has_min_similarity = true; o.min_similarity = static_cast<float>(m);
    }
    o.require_image = j.value("require_image", false);
    int64_t frame_id = 0, region_id = 0; Roi roi; const bool by_region = get_int(j, "region_id", region_id);
    if (by_region) { if (region_id < 1) return jerr(400, "invalid_argument", "region_id must be >= 1"); }
    else {
        if (!get_int(j, "frame_id", frame_id) || frame_id < 1) return jerr(400, "invalid_argument", "region_id, or frame_id with x,y,w,h, required");
        if (!get_roi(j, roi, why)) return jerr(400, "invalid_argument", why);
    }
    NpuGuard ng(a);
    if (!ng.held) return jerr(429, "busy", "another search or indexing operation is running");
    const int64_t t_start = steady_ms();
    std::shared_ptr<EmbeddingProvider> prov;
    if (!by_region) { prov = a.get_provider(why); if (!prov) return jerr(503, "provider_failed", why); }
    ObjectSearchResult res;
    const int cancelled = run_watched(a, fd, [oi] { oi->cancel(); }, [&] {
        res = by_region ? oi->search_by_region(rd, region_id, o) : oi->search_by_roi(*rd, *prov, frame_id, roi, o);
    });
    std::string code = code_name(res.status.code); int http = 200;
    if (res.status.code == SearchStatus::Code::Cancelled) { code = cancelled == 1 ? "timeout" : "cancelled"; http = cancelled == 1 ? 504 : 200; }
    else if (res.status.code == SearchStatus::Code::NotEnabled || res.status.code == SearchStatus::Code::ProviderFailed) http = 503;
    else if (res.status.code == SearchStatus::Code::InvalidArgument || res.status.code == SearchStatus::Code::DimensionMismatch ||
             res.status.code == SearchStatus::Code::InvalidVector || res.status.code == SearchStatus::Code::ModelMismatch) http = 400;
    else if (res.status.code == SearchStatus::Code::DatabaseError) http = 500;
    json hits = json::array(); int rank = 0; for (auto& h : res.hits) hits.push_back(hit_json(h, ++rank));
    const ObjectIndexSummary sm = oi->summary();
    json out = {{"label", kObjLabel},
                {"query", by_region ? json{{"type", "region"}, {"region_id", region_id}}
                                    : json{{"type", "roi"}, {"frame_id", frame_id}, {"bbox", {{"x", roi.x}, {"y", roi.y}, {"w", roi.w}, {"h", roi.h}}}}},
                {"index_regions", sm.regions},
                {"status", {{"code", code}, {"message", res.status.message}, {"elapsed_ms", res.status.elapsed_ms}, {"candidates_scanned", res.index_size},
                            {"total_ms", static_cast<double>(steady_ms() - t_start)}}},
                {"results", hits}};
    if (res.status.code == SearchStatus::Code::Ok && sm.regions == 0) out["status"]["empty_index"] = true;
    Response r = jok(out); r.status = http; return r;
}

Response route(App& a, const Request& rq, int fd) {
    const std::string& p = rq.path;
    if (p.rfind("/api/vm/", 0) != 0) return jerr(404, "not_found", "unknown route");
    const std::string sub = p.substr(8);
    const bool get = rq.method == "GET" || rq.method == "HEAD", post = rq.method == "POST";
    if (sub == "status") return get ? handle_status(a) : jerr(405, "method_not_allowed", "GET only");
    if (sub == "frames") return get ? handle_frames(a, rq) : jerr(405, "method_not_allowed", "GET only");
    if (sub == "search/image") return post ? handle_search(a, rq, fd, false) : jerr(405, "method_not_allowed", "POST only");
    if (sub == "search/frame") return post ? handle_search(a, rq, fd, true) : jerr(405, "method_not_allowed", "POST only");
    if (sub == "objects") {
        if (get) return handle_objects_list(a, rq);
        return post ? handle_object_index(a, rq, fd) : jerr(405, "method_not_allowed", "GET or POST only");
    }
    if (sub == "objects/search") return post ? handle_object_search(a, rq, fd) : jerr(405, "method_not_allowed", "POST only");
    if (sub.rfind("objects/", 0) == 0) {
        if (!get) return jerr(405, "method_not_allowed", "GET only");
        std::string rest = sub.substr(8), tail;
        const size_t sl = rest.find('/');
        if (sl != std::string::npos) { tail = rest.substr(sl + 1); rest = rest.substr(0, sl); }
        int64_t id;
        if (!parse_i64(rest, id) || id < 1) return jerr(400, "invalid_argument", "bad object id");
        if (tail.empty()) return handle_object_detail(a, id);
        if (tail == "image") return handle_object_image(a, id, rq);
        return jerr(404, "not_found", "unknown route");
    }
    if (sub.rfind("frames/", 0) == 0) {
        if (!get) return jerr(405, "method_not_allowed", "GET only");
        std::string rest = sub.substr(7), tail;
        const size_t sl = rest.find('/');
        if (sl != std::string::npos) { tail = rest.substr(sl + 1); rest = rest.substr(0, sl); }
        int64_t id;
        if (!parse_i64(rest, id) || id < 1) return jerr(400, "invalid_argument", "bad frame id");
        if (tail.empty()) return handle_frame_detail(a, id);
        if (tail == "image") return handle_image(a, id, false, rq);
        if (tail == "thumb") return handle_image(a, id, true, rq);
        if (tail == "crop") return handle_frame_crop(a, id, rq);
    }
    return jerr(404, "not_found", "unknown route");
}

std::atomic<int> g_conn{0};

void serve_conn(App& a, int fd) {
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof nodelay);
    timeval tv{10, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    Request rq;
    const int rc = read_request(fd, rq);
    if (rc > 0) send_response(fd, jerr(rc == 413 ? 413 : 400, "bad_request", "malformed or oversized request"));
    else if (rc == 0) {
        try { send_response(fd, route(a, rq, fd), rq.method == "HEAD"); }
        catch (const std::exception& e) { send_response(fd, jerr(500, "internal", "internal error")); }
    }
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
    --g_conn;
}

} // namespace

int main(int argc, char** argv) {
    App app;
    int port = 20310;
    std::string cfg_path;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--config") cfg_path = next();
        else if (a == "--port") port = std::atoi(next().c_str());
        else if (a == "--status-file") app.status_file = next();
        else if (a == "--metrics") app.metrics_addr = next();
        else if (a == "--viewer") app.viewer = true;
        else if (a == "--base-dir") app.base_dir = next();
        else if (a == "--provider") app.provider_kind = next();      // auto | mock (dev/tests only)
        else if (a == "--root") { app.cfg.storage_root = next(); }   // dev: bypass config
        else { std::fprintf(stderr, "usage: vm_api_server --config <argus-config.json> [--port N] [--status-file F] [--base-dir D] [--provider auto|mock]\n"); return 2; }
    }
    if (!cfg_path.empty()) { const std::string root = app.cfg.storage_root; app.cfg = load_cfg(cfg_path); if (!root.empty()) app.cfg.storage_root = root; }
    std::signal(SIGTERM, on_signal); std::signal(SIGINT, on_signal); std::signal(SIGPIPE, SIG_IGN);

    int ls = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);                       // loopback ONLY
    if (::bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(ls, 16) != 0) {
        std::perror("vm_api_server: bind/listen"); return 1;
    }
    std::fprintf(stderr, "vm_api_server: 127.0.0.1:%d configured=%d search=%d config=%s\n", port,
                 app.cfg.configured, app.cfg.vector_search && app.cfg.embedding, app.cfg.config_path.c_str());

    std::thread janitor([&] { while (!g_stop) { for (int i = 0; i < 50 && !g_stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100)); app.maybe_unload(); } });
    while (!g_stop) {
        pollfd p{ls, POLLIN, 0};
        if (::poll(&p, 1, 200) <= 0) continue;
        int fd = ::accept(ls, nullptr, nullptr);
        if (fd < 0) continue;
        if (g_conn.load() >= kMaxConn) { send_response(fd, jerr(503, "busy", "too many connections")); ::close(fd); continue; }
        ++g_conn;
        std::thread([&app, fd] { serve_conn(app, fd); }).detach();
    }
    ::close(ls);
    janitor.join();
    for (int i = 0; i < 50 && g_conn.load() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return 0;
}
