#ifndef RESOURCE_MANAGER_H
#define RESOURCE_MANAGER_H

#include <memory>
#include "resources/resource_types.h"
#include "resources/memory_pool.h"
#include "resources/rga_context.h"
#include "resources/tensor_manager.h"

// Aggregates RGA, scratch pools, and RKNN tensor manager.
// One instance per pipeline is typical.
struct ResourceConfig {
  // Scratch pools (example sizes; tune per model)
  Size2i scratch_nv12{320, 320}; // for letterbox target
  Size2i scratch_rgb {320, 320};

  // Pool sizing
  uint32_t frames_in_flight{2};  // how many concurrent buffers

  // Alignment/pinning
  uint32_t alignment{128};       // DMA-friendly
  bool pinned{true};
};

class ResourceManager {
public:
  static std::unique_ptr<ResourceManager> create(const ResourceConfig& rcfg) noexcept;
  ~ResourceManager();

  ResourceManager(const ResourceManager&) = delete;
  ResourceManager& operator=(const ResourceManager&) = delete;

  // Lifecycle
  bool init_rga() noexcept;
  bool init_scratch_pools() noexcept;      // alloc Y/UV/RGB pools
  bool init_rknn(const char* model_path,
                 const TensorDesc& input,
                 const std::vector<TensorDesc>& outputs) noexcept;

  void unload_rknn() noexcept;

  // Accessors
  RgaContext* rga() noexcept { return rga_.get(); }
  RknnTensorManager* rknn() noexcept { return rknn_.get(); }

  // Scratch acquisitions (caller releases to the same pool)
  bool acquire_nv12_scratch(ImageBuffer& out) noexcept;
  bool acquire_rgb_scratch (ImageBuffer& out) noexcept;
  void release_scratch(ImageBuffer& buf) noexcept;

  // Utility: compute letterbox ROI given src & dst sizes (no HW)
  static void compute_letterbox_roi(const Size2i& src,
                                    const Size2i& dst,
                                    Rect2i& roi_out,
                                    float& scale_out) noexcept;

private:
  explicit ResourceManager(const ResourceConfig& rcfg) noexcept : cfg_(rcfg) {}

  ResourceConfig cfg_{};

  // RGA
  std::unique_ptr<RgaContext> rga_;

  // Scratch pools (NV12 Y + UV, and RGB)
  std::unique_ptr<FixedBlockPool> pool_nv12_y_;
  std::unique_ptr<FixedBlockPool> pool_nv12_uv_;
  std::unique_ptr<FixedBlockPool> pool_rgb_;

  // RKNN tensors
  std::unique_ptr<RknnTensorManager> rknn_;
};

#endif // RESOURCE_MANAGER_H
