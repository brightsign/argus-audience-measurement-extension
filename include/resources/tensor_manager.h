#ifndef TENSOR_MANAGER_H
#define TENSOR_MANAGER_H

#include <cstdint>
#include <memory>
#include <vector>
#include "resource_types.h"

// Opaque RKNN tensor manager: pre-allocates and reuses input/output buffers.
// Keeps RKNN headers out of public headers.
struct TensorDesc {
  // Logical dimensions (NHWC or NCHW as your ModelSpec dictates)
  int n{1}, h{0}, w{0}, c{0};
  bool nhwc{true};
  // Quantization (optional)
  bool  quantized{true};
  float scale{1.f};
  float zero_point{0.f};
  // Element size (1 for u8, 4 for fp32, etc.)
  int elem_bytes{1};
};

class RknnTensorManager {
public:
  static std::unique_ptr<RknnTensorManager> create() noexcept;
  ~RknnTensorManager();

  RknnTensorManager(const RknnTensorManager&) = delete;
  RknnTensorManager& operator=(const RknnTensorManager&) = delete;

  // Initialize RKNN context and allocate IO tensors as per model
  bool init_from_model(const char* model_path,
                       const TensorDesc& input,
                       const std::vector<TensorDesc>& outputs) noexcept;

  // Get raw mapped pointers for input/output tensors (valid until unload)
  uint8_t* input_ptr() noexcept;
  uint8_t* output_ptr(int index) noexcept; // for models with a single packed output, index=0

  int      num_outputs() const noexcept;

  // Optional reshape (if supported by model) without reloading the .rknn
  bool reshape_input(int new_w, int new_h) noexcept;

  // Release RKNN context and tensors
  void unload() noexcept;

private:
  RknnTensorManager() = default;
  struct Impl;
  std::unique_ptr<Impl> p_;
};

#endif // TENSOR_MANAGER_H

