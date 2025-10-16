#ifndef RGA_CONTEXT_H
#define RGA_CONTEXT_H

#include <cstdint>
#include <memory>
#include "resource_types.h"

// Hardware-accelerated image ops via Rockchip RGA with SW fallback.
// Heavy RGA headers are hidden in Impl.
class RgaContext {
public:
  static std::unique_ptr<RgaContext> create() noexcept;
  ~RgaContext();

  RgaContext(const RgaContext&) = delete;
  RgaContext& operator=(const RgaContext&) = delete;

  // Capability flags (queried from driver once)
  bool hw_available() const noexcept;
  bool supports_nv12_to_rgb() const noexcept;
  bool supports_resize() const noexcept;

  // Convert NV12 -> {RGB24,BGR24,GRAY8}; out must be preallocated
  OpResult nv12_to_rgb(const FrameView& in_nv12, ImageBuffer& out_rgb) noexcept;

  // Resize (same fmt in/out). out preallocated to target size/stride.
  OpResult resize(const FrameView& in, ImageBuffer& out) noexcept;

  // Letterbox into out (target size/format). Returns used ROI and scale.
  OpResult letterbox(const FrameView& in,
                     ImageBuffer& out,
                     Rect2i& used_roi,
                     float& scale) noexcept;

private:
  RgaContext() = default;
  struct Impl;
  std::unique_ptr<Impl> p_;
};

#endif // RGA_CONTEXT_H

