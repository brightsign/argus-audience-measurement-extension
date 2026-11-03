// SPDX-License-Identifier: MIT
#include "file_store.h"

#include <atomic>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <random>
#include <system_error>
#include <unistd.h>

namespace fs = std::filesystem;

namespace visualmemory::detail {
namespace {

// Replace characters that are unsafe in a path segment with '_'. Keeps the
// identifier readable while preventing traversal or separators.
std::string sanitise_segment(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '.';
        out.push_back(ok ? c : '_');
    }
    if (out.empty() || out == "." || out == "..") out = "src";
    return out;
}

std::string two(int v) {
    char b[3];
    std::snprintf(b, sizeof(b), "%02d", v);
    return b;
}

uint64_t random64() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    return rng();
}

} // namespace

FileStore::FileStore(std::string root) : root_(std::move(root)) {}

bool FileStore::init() {
    std::error_code ec;
    fs::create_directories(fs::path(root_) / "frames", ec);
    if (ec) return false;
    // Confirm writability with a probe file.
    const fs::path probe = fs::path(root_) / ".vm_write_probe";
    {
        FILE* fp = std::fopen(probe.c_str(), "wb");
        if (!fp) return false;
        std::fclose(fp);
    }
    fs::remove(probe, ec);
    return true;
}

std::string FileStore::make_relative_path(const std::string& source_id,
                                          int64_t capture_utc_ms,
                                          uint64_t seq) const {
    std::time_t t = static_cast<std::time_t>(capture_utc_ms / 1000);
    std::tm tmv{};
#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[512];
    std::snprintf(buf, sizeof(buf), "frames/%04d/%s/%s/%s/%lld_%llu_%016llx.jpg",
                  tmv.tm_year + 1900, two(tmv.tm_mon + 1).c_str(),
                  two(tmv.tm_mday).c_str(), sanitise_segment(source_id).c_str(),
                  static_cast<long long>(capture_utc_ms),
                  static_cast<unsigned long long>(seq),
                  static_cast<unsigned long long>(random64()));
    return buf;
}

bool FileStore::is_safe_relative(const std::string& rel) const {
    if (rel.empty()) return false;
    fs::path p(rel);
    if (p.is_absolute()) return false;
    for (const auto& part : p) {
        if (part == "..") return false;
    }
    return true;
}

std::string FileStore::absolute(const std::string& rel) const {
    return (fs::path(root_) / rel).string();
}

bool FileStore::write_atomic(const std::string& rel, const std::vector<uint8_t>& bytes) {
    if (!is_safe_relative(rel)) return false;
    const fs::path final_path = fs::path(root_) / rel;
    std::error_code ec;
    fs::create_directories(final_path.parent_path(), ec);
    if (ec) return false;

    const fs::path tmp_path = fs::path(final_path.string() + ".tmp");

    int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    size_t written = 0;
    while (written < bytes.size()) {
        ssize_t n = ::write(fd, bytes.data() + written, bytes.size() - written);
        if (n < 0) { ::close(fd); fs::remove(tmp_path, ec); return false; }
        written += static_cast<size_t>(n);
    }
    ::fsync(fd);
    ::close(fd);

    fs::rename(tmp_path, final_path, ec);
    if (ec) { fs::remove(tmp_path, ec); return false; }

    // Best-effort durability of the rename by syncing the directory entry.
    int dfd = ::open(final_path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) { ::fsync(dfd); ::close(dfd); }
    return true;
}

bool FileStore::remove(const std::string& rel) {
    if (!is_safe_relative(rel)) return false;
    std::error_code ec;
    fs::remove(fs::path(root_) / rel, ec);
    return !ec;  // missing file -> remove() returns false but ec is clear
}

int FileStore::reap_temp_files() {
    int removed = 0;
    std::error_code ec;
    const fs::path frames = fs::path(root_) / "frames";
    if (!fs::exists(frames, ec)) return 0;
    for (auto it = fs::recursive_directory_iterator(
             frames, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().extension() == ".tmp") {
            std::error_code rec;
            fs::remove(it->path(), rec);
            if (!rec) ++removed;
        }
    }
    return removed;
}

} // namespace visualmemory::detail
