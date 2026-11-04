// SPDX-License-Identifier: MIT
#include "search_core.h"

#include "embedding_math.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace visualmemory::detail {
namespace {
void fail(SearchStatus& st, SearchStatus::Code c, std::string m) { st.code = c; st.message = std::move(m); }
}

std::vector<SearchResult> search_core(SqliteStore& db, VectorStore& vs, const FileStore* files,
                                      const ModelInfo& m, const std::vector<float>& embedding,
                                      const SearchOptions& o, SearchStatus& st,
                                      const EmbeddingSpec* query_space, int64_t exclude_frame_id) {
    std::vector<SearchResult> results;
    if (query_space) {
        const EmbeddingSpec& a = *query_space; const EmbeddingSpec& b = m.spec;
        if (a.model_name != b.model_name || a.model_version != b.model_version ||
            a.model_sha256 != b.model_sha256 || a.preprocessing != b.preprocessing ||
            a.dimension != b.dimension || a.dtype != b.dtype || a.metric != b.metric) {
            fail(st, SearchStatus::Code::ModelMismatch,
                 "query embedding space " + a.model_name + "/" + a.model_version + "/" +
                 a.preprocessing + " != stored " + b.model_name + "/" + b.model_version + "/" +
                 b.preprocessing);
            return results;
        }
    }
    std::vector<float> q = embedding;
    const VecCheck vc = l2_normalize(q, m.spec.dimension);
    if (vc == VecCheck::BadDimension) {
        fail(st, SearchStatus::Code::DimensionMismatch,
             "query dimension " + std::to_string(embedding.size()) + " != stored " +
             std::to_string(m.spec.dimension));
        return results;
    }
    if (vc != VecCheck::Ok) {
        fail(st, SearchStatus::Code::InvalidVector, std::string("invalid query vector: ") + to_string(vc));
        return results;
    }
    if (o.top_k < 1) { fail(st, SearchStatus::Code::InvalidArgument, "top_k must be >= 1"); return results; }
    if (o.has_min_similarity && !std::isfinite(o.min_similarity)) {
        fail(st, SearchStatus::Code::InvalidArgument, "min_similarity not finite");
        return results;
    }
    const int k = std::min(o.top_k, 100);
    int kq = o.require_image ? std::min(k * 4, 400) : k;   // over-fetch when post-filtering
    if (exclude_frame_id > 0) kq = std::min(kq + 1, 401);

    std::vector<KnnHit> hits;
    std::string err;
    if (!vs.knn(m, q.data(), kq, o.start_utc_ms, o.end_utc_ms, o.source_id, hits, &st.candidates_scanned, err)) {
        const bool intr = err.find("interrupt") != std::string::npos;
        fail(st, intr ? SearchStatus::Code::Cancelled : SearchStatus::Code::DatabaseError, err);
        return results;
    }
    for (const auto& h : hits) {
        if (exclude_frame_id > 0 && h.frame_id == exclude_frame_id) continue;
        const float sim = 1.0f - h.distance;
        if (o.has_min_similarity && sim < o.min_similarity) continue;
        FrameInfo fi;
        if (!db.get_by_id(h.frame_id, fi)) continue;     // vector without a frame row: skip
        SearchResult r;
        r.frame_id = fi.id; r.metadata = fi.metadata; r.relative_jpeg_path = fi.relative_jpeg_path;
        r.image_available = fi.image_available;
        r.absolute_path = files ? files->absolute(fi.relative_jpeg_path) : std::string{};
        std::error_code ec;
        r.image_exists = fi.image_available && !r.absolute_path.empty() &&
                         std::filesystem::exists(r.absolute_path, ec);
        r.model_id = m.model_id; r.similarity = sim; r.distance = h.distance;
        if (o.require_image && !r.image_exists) continue;
        results.push_back(std::move(r));
        if (static_cast<int>(results.size()) >= k) break;
    }
    std::stable_sort(results.begin(), results.end(), [](const SearchResult& a, const SearchResult& b) {
        return a.similarity != b.similarity ? a.similarity > b.similarity : a.frame_id < b.frame_id; });
    st.code = SearchStatus::Code::Ok;
    return results;
}

} // namespace visualmemory::detail
