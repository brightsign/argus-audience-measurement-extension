// SPDX-License-Identifier: MIT
#include "rknn_embedding_provider.h"

#include "rknn_api.h"
#include "sha256.h"
#include "visualmemory/latency_ring.h"
#include "visualmemory/preprocess.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace visualmemory {

struct RknnEmbeddingProvider::Impl {
    Options opt;
    // `mu` serialises the (slow) RKNN context use: init/embed/shutdown. It is HELD during inference,
    // so anything a monitoring/capture thread may call (ready, last_error, provider_stats) must NOT take it.
    mutable std::mutex mu;
    rknn_context ctx = 0;
    bool loaded = false;
    std::atomic<bool> ready_flag{false};
    std::atomic<uint64_t> failures{0};
    mutable std::mutex err_mu;       // guards `err` only; never held across inference
    int dim = 0;
    std::string sha256, err, desc;
    rknn_tensor_attr in_attr{}, out_attr{};
    Pp1Preprocessor pre;
    std::vector<float> in_buf;       // reused NHWC input tensor
    std::vector<float> out_buf;      // reused float output
    LatencyRing lat_pre, lat_inf, lat_post;
    std::vector<uint8_t> model_bytes;   // RKNN keeps no copy for rknn_init(ptr): hold until destroy

    void set_err(const std::string& e) { { std::lock_guard<std::mutex> lk(err_mu); err = e; } failures.fetch_add(1); }
    void set_init_err(const std::string& e) { std::lock_guard<std::mutex> lk(err_mu); err = e; }
};

RknnEmbeddingProvider::RknnEmbeddingProvider(Options opt) : impl_(std::make_unique<Impl>()) {
    impl_->opt = std::move(opt);
}
RknnEmbeddingProvider::~RknnEmbeddingProvider() { shutdown(); }

bool RknnEmbeddingProvider::init() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    Impl& d = *impl_;
    if (d.loaded) return true;
    FILE* f = std::fopen(d.opt.model_path.c_str(), "rb");
    if (!f) { d.set_init_err("cannot open model " + d.opt.model_path); return false; }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (256L << 20)) { std::fclose(f); d.set_init_err("model file size out of range"); return false; }
    d.model_bytes.resize(static_cast<size_t>(n));
    if (std::fread(d.model_bytes.data(), 1, d.model_bytes.size(), f) != d.model_bytes.size()) {
        std::fclose(f); d.set_init_err("short read of model"); return false;
    }
    std::fclose(f);
    d.sha256 = detail::sha256_hex(d.model_bytes.data(), d.model_bytes.size());

    int ret = rknn_init(&d.ctx, d.model_bytes.data(), static_cast<uint32_t>(d.model_bytes.size()), 0, nullptr);
    if (ret != RKNN_SUCC) { d.set_init_err("rknn_init failed: " + std::to_string(ret)); d.ctx = 0; d.model_bytes.clear(); return false; }
    auto fail = [&](const std::string& e) {
        d.set_init_err(e); rknn_destroy(d.ctx); d.ctx = 0; d.model_bytes.clear(); return false; };

    if (d.opt.npu_core >= 0) {
        rknn_core_mask m = RKNN_NPU_CORE_AUTO;
        switch (d.opt.npu_core) {
            case 0: m = RKNN_NPU_CORE_0; break;
            case 1: m = RKNN_NPU_CORE_1; break;
            case 2: m = RKNN_NPU_CORE_2; break;
            default: m = RKNN_NPU_CORE_0_1_2; break;
        }
        ret = rknn_set_core_mask(d.ctx, m);
        if (ret != RKNN_SUCC) return fail("rknn_set_core_mask failed: " + std::to_string(ret));
    }
    rknn_input_output_num io{};
    ret = rknn_query(d.ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
    if (ret != RKNN_SUCC || io.n_input != 1 || io.n_output != 1)
        return fail("unexpected model I/O count (need 1 input, 1 output)");
    std::memset(&d.in_attr, 0, sizeof(d.in_attr));
    d.in_attr.index = 0;
    if (rknn_query(d.ctx, RKNN_QUERY_INPUT_ATTR, &d.in_attr, sizeof(d.in_attr)) != RKNN_SUCC)
        return fail("rknn_query(INPUT_ATTR) failed");
    std::memset(&d.out_attr, 0, sizeof(d.out_attr));
    d.out_attr.index = 0;
    if (rknn_query(d.ctx, RKNN_QUERY_OUTPUT_ATTR, &d.out_attr, sizeof(d.out_attr)) != RKNN_SUCC)
        return fail("rknn_query(OUTPUT_ATTR) failed");
    // Do not assume layout/size: verify the model matches preprocessing pp1.
    if (d.in_attr.fmt != RKNN_TENSOR_NHWC || d.in_attr.n_elems != kPp1Floats)
        return fail("model input is not NHWC 224x224x3 (n_elems=" + std::to_string(d.in_attr.n_elems) +
                    ", fmt=" + std::to_string(static_cast<int>(d.in_attr.fmt)) + ")");
    d.dim = static_cast<int>(d.out_attr.n_elems);
    if (d.dim <= 0 || d.dim > 8192) return fail("implausible output dimension " + std::to_string(d.dim));
    if (d.opt.expected_dimension > 0 && d.dim != d.opt.expected_dimension)
        return fail("output dimension " + std::to_string(d.dim) + " != expected " +
                    std::to_string(d.opt.expected_dimension));
    d.in_buf.assign(kPp1Floats, 0.0f);
    d.out_buf.assign(static_cast<size_t>(d.dim), 0.0f);

    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "input dims=[%u,%u,%u,%u] fmt=%s type=%s qnt=%s scale=%g zp=%d | output n_elems=%u type=%s "
                  "qnt=%s scale=%g zp=%d",
                  d.in_attr.dims[0], d.in_attr.dims[1], d.in_attr.dims[2], d.in_attr.dims[3],
                  get_format_string(d.in_attr.fmt), get_type_string(d.in_attr.type), get_qnt_type_string(d.in_attr.qnt_type),
                  d.in_attr.scale, d.in_attr.zp, d.out_attr.n_elems, get_type_string(d.out_attr.type),
                  get_qnt_type_string(d.out_attr.qnt_type), d.out_attr.scale, d.out_attr.zp);
    d.desc = buf;
    d.loaded = true;
    d.ready_flag.store(true);
    d.set_init_err("");
    return true;
}

EmbeddingSpec RknnEmbeddingProvider::spec() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    EmbeddingSpec s;
    s.model_name = impl_->opt.model_name;
    s.model_version = impl_->opt.model_version;
    s.dimension = impl_->dim;
    s.preprocessing = kPp1Id;
    s.model_sha256 = impl_->sha256;
    return s;
}

bool RknnEmbeddingProvider::ready() const { return impl_->ready_flag.load(); }   // lock-free

std::string RknnEmbeddingProvider::last_error() const {
    std::lock_guard<std::mutex> lk(impl_->err_mu);
    return impl_->err;
}

std::string RknnEmbeddingProvider::describe() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->desc;
}

EmbeddingProviderStats RknnEmbeddingProvider::provider_stats() const {
    EmbeddingProviderStats s;
    s.preprocess = impl_->lat_pre.snapshot();
    s.inference = impl_->lat_inf.snapshot();
    s.postprocess = impl_->lat_post.snapshot();
    s.failures = impl_->failures.load();
    return s;                        // no impl_->mu: never waits for an in-flight inference
}

bool RknnEmbeddingProvider::embed(const ImageView& image, std::vector<float>& out) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    Impl& d = *impl_;
    if (!d.loaded) { d.set_err("model not loaded"); return false; }
    const int64_t t0 = steady_now_ns();
    std::string perr;
    if (!d.pre.run(image, d.in_buf.data(), perr)) { d.set_err("preprocess: " + perr); return false; }
    const int64_t t1 = steady_now_ns();
    d.lat_pre.add(t1 - t0);

    rknn_input in{};
    in.index = 0;
    in.buf = d.in_buf.data();
    in.size = static_cast<uint32_t>(d.in_buf.size() * sizeof(float));
    in.pass_through = 0;                 // let the runtime quantise/convert per the model's input attr
    in.type = RKNN_TENSOR_FLOAT32;
    in.fmt = RKNN_TENSOR_NHWC;
    int ret = rknn_inputs_set(d.ctx, 1, &in);
    if (ret != RKNN_SUCC) { d.set_err("rknn_inputs_set failed: " + std::to_string(ret)); return false; }
    ret = rknn_run(d.ctx, nullptr);
    if (ret != RKNN_SUCC) { d.set_err("rknn_run failed: " + std::to_string(ret)); return false; }
    rknn_output o{};
    o.index = 0;
    o.want_float = 1;                    // runtime dequantises INT8 output
    o.is_prealloc = 1;
    o.buf = d.out_buf.data();
    o.size = static_cast<uint32_t>(d.out_buf.size() * sizeof(float));
    ret = rknn_outputs_get(d.ctx, 1, &o, nullptr);
    if (ret != RKNN_SUCC) { d.set_err("rknn_outputs_get failed: " + std::to_string(ret)); return false; }
    const int64_t t2 = steady_now_ns();
    d.lat_inf.add(t2 - t1);
    rknn_outputs_release(d.ctx, 1, &o);
    out.assign(d.out_buf.begin(), d.out_buf.end());
    d.lat_post.add(steady_now_ns() - t2);
    return true;
}

void RknnEmbeddingProvider::shutdown() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    Impl& d = *impl_;
    d.ready_flag.store(false);
    if (d.loaded || d.ctx) { rknn_destroy(d.ctx); d.ctx = 0; }
    d.loaded = false;
    d.model_bytes.clear(); d.model_bytes.shrink_to_fit();
    d.in_buf.clear(); d.in_buf.shrink_to_fit();
}

} // namespace visualmemory
