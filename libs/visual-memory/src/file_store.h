// SPDX-License-Identifier: MIT
// Internal: filesystem persistence for encoded JPEGs. All paths are confined to
// the configured root; traversal is rejected.
#ifndef VISUALMEMORY_FILE_STORE_H
#define VISUALMEMORY_FILE_STORE_H

#include "visualmemory/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace visualmemory::detail {

class FileStore {
public:
    explicit FileStore(std::string root);

    // Create the root and frames/ directory. Returns false if not writable.
    bool init();

    // Build a collision-resistant relative path grouped by YYYY/MM/DD/<source>/.
    // `capture_utc_ms` picks the date folder; `source_id` is sanitised.
    std::string make_relative_path(const std::string& source_id,
                                   int64_t capture_utc_ms,
                                   uint64_t seq) const;

    // Atomically write bytes to <root>/<relative>: write to a .tmp sibling,
    // fsync, then rename into place. Creates parent dirs. Returns false on error.
    bool write_atomic(const std::string& relative_path,
                      const std::vector<uint8_t>& bytes);

    // Delete <root>/<relative> if inside the root. Missing file is treated as
    // success. Returns false only on a real filesystem error.
    bool remove(const std::string& relative_path);

    // Absolute path for a relative path (no existence check).
    std::string absolute(const std::string& relative_path) const;

    // Reject absolute paths, "..", and anything escaping the root.
    bool is_safe_relative(const std::string& relative_path) const;

    // Delete stale *.tmp files left by an interrupted write (startup recovery).
    // Returns the number removed.
    int reap_temp_files();

    const std::string& root() const { return root_; }

private:
    std::string root_;
};

} // namespace visualmemory::detail

#endif // VISUALMEMORY_FILE_STORE_H
