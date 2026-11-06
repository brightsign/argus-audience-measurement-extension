// SPDX-License-Identifier: MIT
// VectorStore on SQLite (sqlite-vec vec0 and the BLOB fallback): registry, exact top-K vs a brute-force
// oracle, filters, idempotence, model separation, retention helpers, persistence, reconciliation.
#include "sqlite_store.h"
#include "vector_store.h"
#include "test_util.h"

#include <gtest/gtest.h>
#include <sqlite3.h>
#include <algorithm>
#include <cmath>
#include <random>

using namespace visualmemory;
using namespace visualmemory::detail;

namespace {

InsertRow frame_row(const std::string& src, int64_t cap_ms, uint64_t seq) {
    InsertRow r;
    r.source_id = src; r.source_type = "file"; r.device_id = "dev"; r.capture_utc_ms = cap_ms;
    r.source_sequence = seq; r.stored_width = 64; r.stored_height = 64; r.pixel_format = "RGB24";
    r.relative_jpeg_path = "frames/" + src + "/" + std::to_string(seq) + ".jpg";
    r.jpeg_size_bytes = 1000; r.created_utc_ms = cap_ms;
    return r;
}

std::vector<float> rand_unit(std::mt19937& g, int dim) {
    std::normal_distribution<float> n(0, 1);
    std::vector<float> v(dim); double s = 0;
    for (auto& x : v) { x = n(g); s += double(x) * x; }
    for (auto& x : v) x = static_cast<float>(x / std::sqrt(s));
    return v;
}

EmbeddingSpec spec(int dim, const std::string& ver = "v1", const std::string& pre = "pp1") {
    EmbeddingSpec s; s.model_name = "m"; s.model_version = ver; s.dimension = dim; s.preprocessing = pre;
    s.model_sha256 = "abc"; return s;
}

struct Fixture {
    vmtest::TempDir dir;
    SqliteStore db;
    VectorStore vs{db};
    explicit Fixture(const std::string& name = "t.db") {
        std::string err;
        if (!db.open(dir.str() + "/" + name, err)) throw std::runtime_error(err);
    }
};

class Backend : public ::testing::TestWithParam<bool> {};   // true = vec0, false = BLOB fallback

} // namespace

TEST(VectorStoreCompat, ReportsSqliteAndVecVersions) {
    Fixture f;
    EXPECT_EQ(f.db.sqlite_version().substr(0, 4), std::string(sqlite3_libversion()).substr(0, 4));
#ifdef VM_HAVE_SQLITE_VEC
    EXPECT_TRUE(f.db.vec_available());
    EXPECT_EQ(f.db.vec_version(), "v0.1.6");
#endif
    RecordProperty("sqlite_version", f.db.sqlite_version());
}

TEST(VectorStoreSchema, MigratesV1DatabaseInPlaceAndKeepsRowsReadable) {
    vmtest::TempDir dir;
    const std::string path = dir.str() + "/v1.db";
    {   // hand-build a genuine Stage 1 (user_version=1) database
        sqlite3* db; ASSERT_EQ(sqlite3_open(path.c_str(), &db), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(db,
            "CREATE TABLE frames (id INTEGER PRIMARY KEY, source_id TEXT NOT NULL, source_type TEXT NOT NULL,"
            "device_id TEXT NOT NULL, capture_utc_ms INTEGER NOT NULL, pts_ns INTEGER, source_sequence INTEGER,"
            "stored_width INTEGER NOT NULL, stored_height INTEGER NOT NULL, pixel_format TEXT NOT NULL,"
            "relative_jpeg_path TEXT NOT NULL UNIQUE, jpeg_size_bytes INTEGER NOT NULL, jpeg_sha256 TEXT,"
            "created_utc_ms INTEGER NOT NULL);"
            "INSERT INTO frames(source_id,source_type,device_id,capture_utc_ms,stored_width,stored_height,"
            "pixel_format,relative_jpeg_path,jpeg_size_bytes,created_utc_ms) VALUES('cam','usb','d',5000,640,360,"
            "'BGR24','frames/a.jpg',1234,5000);"
            "PRAGMA user_version=1;", nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(db);
    }
    SqliteStore st; std::string err;
    ASSERT_TRUE(st.open(path, err)) << err;
    auto rows = st.query_by_time(0, 100000, "", 0, 0);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].relative_jpeg_path, "frames/a.jpg");
    EXPECT_TRUE(rows[0].image_available);                 // defaulted for pre-existing rows
    EXPECT_EQ(st.total_jpeg_bytes(), 1234u);
    st.close();
    ASSERT_TRUE(st.open(path, err)) << err;               // re-open: migration is idempotent
    EXPECT_EQ(st.schema_version(), 2);
}

TEST(VectorStoreSchema, Stage1BinaryResettingUserVersionDoesNotBreakMigration) {
    vmtest::TempDir dir;
    const std::string path = dir.str() + "/d.db";
    SqliteStore a; std::string err;
    ASSERT_TRUE(a.open(path, err)) << err;
    a.close();
    sqlite3* db; ASSERT_EQ(sqlite3_open(path.c_str(), &db), SQLITE_OK);   // simulate old binary lowering the version
    sqlite3_exec(db, "PRAGMA user_version=1;", nullptr, nullptr, nullptr); sqlite3_close(db);
    SqliteStore b;
    EXPECT_TRUE(b.open(path, err)) << err;                // must not fail on "duplicate column"
}

TEST_P(Backend, RegisterInsertSearchExactVsOracle) {
    Fixture f; const bool vec = GetParam();
    if (vec && !f.db.vec_available()) GTEST_SKIP() << "sqlite-vec not compiled in";
    const int dim = 48, N = 300;
    ModelInfo m; std::string err;
    ASSERT_TRUE(f.vs.register_model(spec(dim), vec, m, err)) << err;
    EXPECT_EQ(m.backend, vec ? "vec0" : "blob");
    std::mt19937 g(1);
    std::vector<std::vector<float>> all;
    for (int i = 0; i < N; ++i) {
        int64_t id = f.db.insert(frame_row(i % 3 == 0 ? "camA" : "camB", 1000000 + i * 5000, i + 1));
        ASSERT_GT(id, 0);
        all.push_back(rand_unit(g, dim));
        ASSERT_EQ(f.vs.insert(m, id, all.back().data(), 1000000 + i * 5000, i % 3 == 0 ? "camA" : "camB", err),
                  InsertResult::Inserted) << err;
    }
    EXPECT_EQ(f.vs.count(m), static_cast<uint64_t>(N));
    EXPECT_EQ(f.vs.total_vector_bytes(), static_cast<uint64_t>(N) * dim * 4);
    auto q = rand_unit(g, dim);
    // oracle
    std::vector<std::pair<float, int64_t>> orc;
    for (int i = 0; i < N; ++i) {
        double d = 0; for (int k = 0; k < dim; ++k) d += double(q[k]) * all[i][k];
        orc.push_back({static_cast<float>(d), i + 1});
    }
    std::sort(orc.begin(), orc.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<KnnHit> hits;
    ASSERT_TRUE(f.vs.knn(m, q.data(), 10, 0, 0, "", hits, nullptr, err)) << err;
    ASSERT_EQ(hits.size(), 10u);
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(hits[i].frame_id, orc[i].second) << "rank " << i;
        EXPECT_NEAR(1.0f - hits[i].distance, orc[i].first, 1e-4f);
    }
    // exact stored vector retrieval: querying a stored vector returns itself with similarity 1
    ASSERT_TRUE(f.vs.knn(m, all[7].data(), 1, 0, 0, "", hits, nullptr, err));
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].frame_id, 8);
    EXPECT_NEAR(hits[0].distance, 0.0f, 1e-5f);

    // time window [start,end) and source filter agree with the oracle restricted to the same subset
    const int64_t s = 1000000 + 100 * 5000, e = 1000000 + 200 * 5000;
    ASSERT_TRUE(f.vs.knn(m, q.data(), 5, s, e, "", hits, nullptr, err));
    for (auto& h : hits) { EXPECT_GE(h.frame_id, 101); EXPECT_LE(h.frame_id, 200); }
    ASSERT_TRUE(f.vs.knn(m, q.data(), 5, 0, 0, "camA", hits, nullptr, err));
    ASSERT_EQ(hits.size(), 5u);
    for (auto& h : hits) EXPECT_EQ((h.frame_id - 1) % 3, 0);
    ASSERT_TRUE(f.vs.knn(m, q.data(), 5, s, e, "camB", hits, nullptr, err));
    for (auto& h : hits) { EXPECT_NE((h.frame_id - 1) % 3, 0); EXPECT_TRUE(h.frame_id >= 101 && h.frame_id <= 200); }
    ASSERT_TRUE(f.vs.knn(m, q.data(), 5, 0, 0, "no-such-source", hits, nullptr, err));
    EXPECT_TRUE(hits.empty());
    uint64_t scanned = 0;
    ASSERT_TRUE(f.vs.knn(m, q.data(), 5, s, e, "", hits, &scanned, err));
    EXPECT_EQ(scanned, 100u);
}

TEST_P(Backend, InsertIsIdempotentAndFailedMarkerCanBeReplaced) {
    Fixture f; const bool vec = GetParam();
    if (vec && !f.db.vec_available()) GTEST_SKIP();
    ModelInfo m; std::string err;
    ASSERT_TRUE(f.vs.register_model(spec(8), vec, m, err)) << err;
    std::mt19937 g(2);
    auto v = rand_unit(g, 8);
    int64_t id = f.db.insert(frame_row("c", 10, 1));
    EXPECT_EQ(f.vs.insert(m, id, v.data(), 10, "c", err), InsertResult::Inserted);
    EXPECT_EQ(f.vs.insert(m, id, v.data(), 10, "c", err), InsertResult::AlreadyExists);   // no duplicate vector
    EXPECT_EQ(f.vs.count(m), 1u);
    int64_t id2 = f.db.insert(frame_row("c", 20, 2));
    EXPECT_TRUE(f.vs.mark_failed(m, id2, "decode error"));
    EXPECT_EQ(f.vs.count(m), 1u);                                      // failed rows are not vectors
    EXPECT_TRUE(f.vs.frames_missing(m, 0, INT64_MAX, 10).empty());    // ... and are not re-selected for backfill
    EXPECT_EQ(f.vs.insert(m, id2, v.data(), 20, "c", err), InsertResult::Inserted);   // explicit retry replaces marker
    EXPECT_EQ(f.vs.count(m), 2u);
}

TEST_P(Backend, FramesMissingIsNewestFirstAndSkipsUnavailableImages) {
    Fixture f; const bool vec = GetParam();
    if (vec && !f.db.vec_available()) GTEST_SKIP();
    ModelInfo m; std::string err;
    ASSERT_TRUE(f.vs.register_model(spec(8), vec, m, err));
    std::vector<int64_t> ids;
    for (int i = 0; i < 5; ++i) ids.push_back(f.db.insert(frame_row("c", 100 + i, i + 1)));
    f.db.mark_image_unavailable(ids[4]);
    auto miss = f.vs.frames_missing(m, 0, INT64_MAX, 10);
    ASSERT_EQ(miss.size(), 4u);
    EXPECT_EQ(miss[0].id, ids[3]);                     // newest available first
    EXPECT_EQ(miss[3].id, ids[0]);
    EXPECT_EQ(f.vs.frames_missing(m, 0, 102, 10).size(), 2u);   // cursor bound is exclusive
}

TEST_P(Backend, DeleteOlderThanRemovesVectorsAndOrphanedRowsOnly) {
    Fixture f; const bool vec = GetParam();
    if (vec && !f.db.vec_available()) GTEST_SKIP();
    ModelInfo m; std::string err;
    ASSERT_TRUE(f.vs.register_model(spec(8), vec, m, err));
    std::mt19937 g(3);
    std::vector<int64_t> ids;
    for (int i = 0; i < 6; ++i) {
        ids.push_back(f.db.insert(frame_row("c", 1000 + i * 10, i + 1)));
        auto v = rand_unit(g, 8);
        ASSERT_EQ(f.vs.insert(m, ids.back(), v.data(), 1000 + i * 10, "c", err), InsertResult::Inserted);
    }
    f.db.mark_image_unavailable(ids[0]);              // JPEG already expired for frame 0 and 1
    f.db.mark_image_unavailable(ids[1]);
    EXPECT_EQ(f.vs.delete_older_than(1025, 100), 3u);  // frames 0,1,2 (captured < 1025)
    EXPECT_EQ(f.vs.count(m), 3u);
    FrameInfo fi;
    EXPECT_FALSE(f.db.get_by_id(ids[0], fi));          // image gone + embedding gone => row removed
    EXPECT_FALSE(f.db.get_by_id(ids[1], fi));
    EXPECT_TRUE(f.db.get_by_id(ids[2], fi));           // JPEG still available => row stays (just no vector)
    std::vector<KnnHit> hits; auto q = rand_unit(g, 8);
    ASSERT_TRUE(f.vs.knn(m, q.data(), 10, 0, 0, "", hits, nullptr, err));
    EXPECT_EQ(hits.size(), 3u);
    EXPECT_EQ(f.vs.delete_oldest(1), 1u);
    EXPECT_EQ(f.vs.count(m), 2u);
}

TEST_P(Backend, ReconcileRepairsVectorWithoutMarkerAndMarkerWithoutVector) {
    Fixture f; const bool vec = GetParam();
    if (vec && !f.db.vec_available()) GTEST_SKIP();
    ModelInfo m; std::string err;
    ASSERT_TRUE(f.vs.register_model(spec(8), vec, m, err));
    std::mt19937 g(4);
    int64_t a = f.db.insert(frame_row("c", 10, 1)), b = f.db.insert(frame_row("c", 20, 2));
    auto v = rand_unit(g, 8);
    ASSERT_EQ(f.vs.insert(m, a, v.data(), 10, "c", err), InsertResult::Inserted);
    ASSERT_EQ(f.vs.insert(m, b, v.data(), 20, "c", err), InsertResult::Inserted);
    EXPECT_EQ(f.vs.reconcile(m), 0u);                  // consistent => no work
    // corrupt: drop vector row of `a` directly, and its marker of `b` directly
    f.db.with_db([&](sqlite3* db) {
        const std::string t = vec ? "vec_emb_" + std::to_string(m.model_id) : "blob_emb_" + std::to_string(m.model_id);
        const std::string key = vec ? "rowid" : "frame_id";
        sqlite3_exec(db, ("DELETE FROM " + t + " WHERE " + key + "=" + std::to_string(a)).c_str(), nullptr, nullptr, nullptr);
        sqlite3_exec(db, ("DELETE FROM frame_embeddings WHERE frame_id=" + std::to_string(b)).c_str(), nullptr, nullptr, nullptr);
        return 0; });
    EXPECT_EQ(f.vs.reconcile(m), 2u);
    EXPECT_EQ(f.vs.count(m), 0u);
    EXPECT_EQ(f.vs.frames_missing(m, 0, INT64_MAX, 10).size(), 2u);   // both are eligible for re-embedding
}

INSTANTIATE_TEST_SUITE_P(VecAndBlob, Backend, ::testing::Values(true, false),
                         [](const auto& i) { return i.param ? "sqlite_vec" : "blob_fallback"; });

TEST(VectorStoreModels, DifferentSpacesNeverShareATable) {
    Fixture f;
    ModelInfo a, b, a2; std::string err;
    ASSERT_TRUE(f.vs.register_model(spec(16, "v1", "pp1"), true, a, err));
    ASSERT_TRUE(f.vs.register_model(spec(16, "v2", "pp1"), true, b, err));     // new model version
    ASSERT_TRUE(f.vs.register_model(spec(16, "v1", "pp2"), true, a2, err));    // new preprocessing version
    EXPECT_NE(a.model_id, b.model_id);
    EXPECT_NE(a.model_id, a2.model_id);
    ModelInfo again;
    ASSERT_TRUE(f.vs.register_model(spec(16, "v1", "pp1"), true, again, err));
    EXPECT_EQ(again.model_id, a.model_id);                                       // registration is idempotent
    std::mt19937 g(5);
    int64_t id = f.db.insert(frame_row("c", 10, 1));
    auto v = rand_unit(g, 16);
    ASSERT_EQ(f.vs.insert(a, id, v.data(), 10, "c", err), InsertResult::Inserted);
    std::vector<KnnHit> hits;
    ASSERT_TRUE(f.vs.knn(b, v.data(), 5, 0, 0, "", hits, nullptr, err));
    EXPECT_TRUE(hits.empty());                                                   // other model's space is empty
    EXPECT_EQ(f.vs.models().size(), 3u);
}

TEST(VectorStoreModels, RejectsUnsupportedSpecs) {
    Fixture f; ModelInfo m; std::string err;
    EmbeddingSpec s = spec(0);
    EXPECT_FALSE(f.vs.register_model(s, true, m, err));
    s = spec(8); s.dtype = "i8";
    EXPECT_FALSE(f.vs.register_model(s, true, m, err));
    s = spec(8); s.metric = "l2";
    EXPECT_FALSE(f.vs.register_model(s, true, m, err));
}

TEST(VectorStorePersistence, VectorsAndModelsSurviveReopen) {
    vmtest::TempDir dir; std::string err; const std::string path = dir.str() + "/p.db";
    std::mt19937 g(6); std::vector<float> v;
    int64_t id; int mid;
    {
        SqliteStore db; ASSERT_TRUE(db.open(path, err)); VectorStore vs(db); ModelInfo m;
        ASSERT_TRUE(vs.register_model(spec(24), true, m, err)); mid = static_cast<int>(m.model_id);
        id = db.insert(frame_row("c", 10, 1)); v = rand_unit(g, 24);
        ASSERT_EQ(vs.insert(m, id, v.data(), 10, "c", err), InsertResult::Inserted);
    }
    SqliteStore db; ASSERT_TRUE(db.open(path, err)); VectorStore vs(db); ModelInfo m;
    ASSERT_TRUE(vs.register_model(spec(24), true, m, err));
    EXPECT_EQ(m.model_id, mid);
    EXPECT_EQ(vs.count(m), 1u);
    std::vector<KnnHit> hits;
    ASSERT_TRUE(vs.knn(m, v.data(), 1, 0, 0, "", hits, nullptr, err));
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].frame_id, id);
    EXPECT_NEAR(hits[0].distance, 0.0f, 1e-5f);
}

TEST(VectorStoreScale, TopKStaysExactAt1000And10000Vectors) {
    for (size_t N : {100u, 1000u, 10000u}) {
        Fixture f; ModelInfo m; std::string err;
        const int dim = 32;
        ASSERT_TRUE(f.vs.register_model(spec(dim), true, m, err)) << err;
        std::mt19937 g(11);
        std::vector<std::vector<float>> all;
        // single outer transaction per batch would be faster, but VectorStore::insert is the API under test
        for (size_t i = 0; i < N; ++i) {
            int64_t id = f.db.insert(frame_row("c", 1000 + int64_t(i), i + 1));
            all.push_back(rand_unit(g, dim));
            ASSERT_EQ(f.vs.insert(m, id, all.back().data(), 1000 + int64_t(i), "c", err), InsertResult::Inserted) << err;
        }
        EXPECT_EQ(f.vs.count(m), N);
        auto q = rand_unit(g, dim);
        double best = -2; size_t bi = 0;
        for (size_t i = 0; i < N; ++i) {
            double d = 0; for (int k = 0; k < dim; ++k) d += double(q[k]) * all[i][k];
            if (d > best) { best = d; bi = i; }
        }
        std::vector<KnnHit> hits;
        ASSERT_TRUE(f.vs.knn(m, q.data(), 3, 0, 0, "", hits, nullptr, err)) << err;
        ASSERT_FALSE(hits.empty());
        EXPECT_EQ(hits[0].frame_id, static_cast<int64_t>(bi + 1)) << "N=" << N;
    }
}
