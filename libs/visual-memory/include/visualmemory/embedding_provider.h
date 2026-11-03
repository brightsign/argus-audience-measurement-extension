// SPDX-License-Identifier: MIT
//
// libvisualmemory — Stage 2 embedding interface (DECLARATION ONLY).
//
// This is the clean extension point for future image embeddings / vector
// similarity. Stage 1 does NOT implement or require any of this. No RKNN, ONNX
// or model dependency is pulled in by including this header.
#ifndef VISUALMEMORY_EMBEDDING_PROVIDER_H
#define VISUALMEMORY_EMBEDDING_PROVIDER_H

#include "visualmemory/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace visualmemory {

// Describes the vector space produced by an EmbeddingProvider. Vectors from
// different (model_name, model_version, dimension, dtype) spaces must never be
// compared.
struct EmbeddingSpec {
    std::string model_name;
    std::string model_version;
    int         dimension = 0;
    std::string dtype = "f32";   // "f32" | "i8" | ...
    std::string preprocessing;   // free-form record of normalisation applied
};

// Abstract provider that turns an owned image/crop into a feature vector.
// Implementations (CPU reference, RKNN backend, ...) live OUTSIDE the Stage 1
// core and are compiled only when ENABLE_VISUAL_MEMORY_VECTORS is ON.
class EmbeddingProvider {
public:
    virtual ~EmbeddingProvider() = default;
    virtual EmbeddingSpec spec() const = 0;
    // Extract a feature vector from an owned frame. Returns false on failure.
    virtual bool extract(const OwnedFrame& frame, std::vector<float>& out) = 0;
};

} // namespace visualmemory

#endif // VISUALMEMORY_EMBEDDING_PROVIDER_H
