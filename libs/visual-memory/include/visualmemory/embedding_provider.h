// SPDX-License-Identifier: MIT
//
// libvisualmemory — embedding provider interface (Stage 2).
//
// A provider turns an 8-bit RGB/BGR image into a feature vector. Implementations
// (mock, RKNN NPU, ...) live OUTSIDE the core library; the core only depends on
// this header, so no RKNN/ONNX/model dependency is ever pulled in by including it.
//
// Contract
//  * embed() must be safe to call from several threads (the embedding worker and
//    search_by_image() callers). Implementations serialise access to any single
//    device context internally.
//  * embed() may return raw (un-normalised) features; the core always flattens,
//    validates (dimension, NaN/Inf, near-zero norm) and L2-normalises the result
//    before storing or comparing it.
//  * A provider whose model failed to load reports ready()==false and fails every
//    embed() call cleanly; the library then simply runs without Stage 2.
#ifndef VISUALMEMORY_EMBEDDING_PROVIDER_H
#define VISUALMEMORY_EMBEDDING_PROVIDER_H

#include "visualmemory/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace visualmemory {

// Identity of the vector space produced by a provider. Vectors from different
// spaces (any field differing) are NEVER compared: the store keeps one vector
// table per distinct identity.
struct EmbeddingSpec {
    std::string model_name;
    std::string model_version;
    int         dimension = 0;
    std::string dtype = "f32";   // storage type of the persisted vector ("f32")
    std::string preprocessing;   // preprocessing id/version, e.g. "pp1"
    std::string model_sha256;    // hash of the deployed model file ("" if unknown)
    std::string metric = "cosine";
};

// Per-stage latency of the provider's own work (all optional).
struct EmbeddingProviderStats {
    LatencyStats preprocess;     // resize / normalise / layout conversion
    LatencyStats inference;      // device run (e.g. rknn_run) incl. input/output transfer
    LatencyStats postprocess;    // dequantise / copy-out
    uint64_t     failures = 0;
};

class EmbeddingProvider {
public:
    virtual ~EmbeddingProvider() = default;

    // Identity + dimension of the produced vectors. Stable for the provider's life.
    virtual EmbeddingSpec spec() const = 0;

    // Extract a feature vector from a decoded RGB24/BGR24 image. Returns false on
    // failure (see last_error()). Thread-safe.
    virtual bool embed(const ImageView& image, std::vector<float>& out) = 0;

    // Convenience kept from the Stage 1 declaration: owned RGB24/BGR24 frames.
    virtual bool extract(const OwnedFrame& frame, std::vector<float>& out) {
        if (frame.pixel_format != PixelFormat::RGB24 && frame.pixel_format != PixelFormat::BGR24)
            return false;
        ImageView v;
        v.data = frame.pixels.data(); v.width = frame.width; v.height = frame.height;
        v.stride = frame.stride0; v.format = frame.pixel_format;
        return embed(v, out);
    }

    // True when the model is loaded and embed() can succeed.
    virtual bool ready() const { return true; }
    // Human-readable description of the most recent failure ("" if none).
    virtual std::string last_error() const { return {}; }
    virtual EmbeddingProviderStats provider_stats() const { return {}; }
    // Release device resources. Idempotent; embed() fails cleanly afterwards.
    virtual void shutdown() {}
};

} // namespace visualmemory

#endif // VISUALMEMORY_EMBEDDING_PROVIDER_H
