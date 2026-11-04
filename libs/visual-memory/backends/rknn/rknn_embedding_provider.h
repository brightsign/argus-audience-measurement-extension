// SPDX-License-Identifier: MIT
//
// RKNN embedding provider (optional backend; NOT a dependency of libvisualmemory).
// Runs the MobileNetV2 feature extractor on a Rockchip NPU through the RKNN C API.
//
// Input contract (must match scripts/convert_rknn.py): float32 NHWC [1,224,224,3], already
// normalised by preprocessing "pp1" (visualmemory/preprocess.h), submitted with
// pass_through=0 so the runtime applies the model's own input quantisation (FP16 or INT8).
// Output: 1280-float pooled feature vector, requested as float32 (want_float=1) so the runtime
// dequantises INT8 outputs. The core library then L2-normalises and validates.
//
// Threading: one RKNN context, access serialised by an internal mutex.
#ifndef VISUALMEMORY_RKNN_EMBEDDING_PROVIDER_H
#define VISUALMEMORY_RKNN_EMBEDDING_PROVIDER_H

#include "visualmemory/embedding_provider.h"

#include <memory>
#include <string>

namespace visualmemory {

class RknnEmbeddingProvider : public EmbeddingProvider {
public:
    struct Options {
        std::string model_path;
        std::string model_name    = "mobilenet_v2_embedder";
        std::string model_version = "tv-imagenet1k-v1";
        int  npu_core = -1;            // -1 auto; 0,1,2 single core; 3 = cores 0+1+2
        int  expected_dimension = 0;   // 0 = take it from the model's output tensor
    };

    explicit RknnEmbeddingProvider(Options opt);
    ~RknnEmbeddingProvider() override;

    // Load the model ONCE and allocate reusable buffers. Returns false (ready()==false,
    // last_error() set) on any failure; callers keep running without embeddings.
    bool init();

    EmbeddingSpec spec() const override;
    bool embed(const ImageView& image, std::vector<float>& out) override;
    bool ready() const override;
    std::string last_error() const override;
    EmbeddingProviderStats provider_stats() const override;
    void shutdown() override;

    // Describes the loaded model for diagnostics (input/output tensor attrs, quant type).
    std::string describe() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace visualmemory

#endif // VISUALMEMORY_RKNN_EMBEDDING_PROVIDER_H
