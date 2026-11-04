// SPDX-License-Identifier: MIT
// Internal: the exact top-K similarity search shared by VisualMemory (writer process) and
// VisualMemoryReader (read-only second process). Pure function of the store + query vector.
#ifndef VISUALMEMORY_SEARCH_CORE_H
#define VISUALMEMORY_SEARCH_CORE_H

#include "file_store.h"
#include "vector_store.h"
#include "visualmemory/types.h"

#include <vector>

namespace visualmemory::detail {

// Validates the query (space, dimension, finite, non-zero), runs the exact scan, resolves frame rows.
// `files` may be null (then image_exists=false, absolute_path empty). `exclude_frame_id` (>0) drops that
// frame from the results (used by "find similar to stored frame"). Fills `st`.
std::vector<SearchResult> search_core(SqliteStore& db, VectorStore& vs, const FileStore* files,
                                      const ModelInfo& m, const std::vector<float>& embedding,
                                      const SearchOptions& o, SearchStatus& st,
                                      const EmbeddingSpec* query_space, int64_t exclude_frame_id = 0);

} // namespace visualmemory::detail
#endif
