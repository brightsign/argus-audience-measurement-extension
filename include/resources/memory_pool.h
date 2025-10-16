#ifndef MEM_POOL_H
#define MEM_POOL_H

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <vector>
#include <memory>
#include "resource_types.h"

// Fixed-size block pool; alignment/pinning are hints honored by implementation.
struct PoolParams {
  size_t block_bytes{0};
  uint32_t blocks{0};
  uint32_t alignment{64};   // cacheline / DMA alignment
  bool pinned{true};        // try to allocate page-locked / DMA-safe
};

class FixedBlockPool {
public:
  static std::unique_ptr<FixedBlockPool> create(const PoolParams& p) noexcept;

  ~FixedBlockPool();

  // non-copyable
  FixedBlockPool(const FixedBlockPool&) = delete;
  FixedBlockPool& operator=(const FixedBlockPool&) = delete;

  // Acquire/release a raw block
  void* acquire() noexcept;                 // nullptr if none available
  void  release(void* ptr) noexcept;

  // Introspection
  uint32_t capacity()  const noexcept { return capacity_; }
  uint32_t available() const noexcept { return available_.load(std::memory_order_relaxed); }
  uint32_t block_size() const noexcept { return static_cast<uint32_t>(block_bytes_); }
  bool     pinned()     const noexcept { return pinned_; }
  uint32_t alignment()  const noexcept { return alignment_; }

private:
  FixedBlockPool() = default;

  size_t block_bytes_{0};
  uint32_t capacity_{0};
  uint32_t alignment_{64};
  bool pinned_{false};

  std::vector<void*> free_; // lock-free-ish via atomic index
  std::atomic<uint32_t> top_{0};
  std::atomic<uint32_t> available_{0};

  // backing storage (if needed for cleanup); impl-defined
  struct Impl;
  std::unique_ptr<Impl> p_;
};

// Convenience: acquire a bi-planar NV12 image from two pools
inline bool acquire_nv12(FixedBlockPool& y_pool,
                         FixedBlockPool& uv_pool,
                         int w, int h, ImageBuffer& out) noexcept {
  void* y = y_pool.acquire();
  void* uv = uv_pool.acquire();
  if (!y || !uv) {
    if (y) y_pool.release(y);
    if (uv) uv_pool.release(uv);
    return false;
  }
  out.fmt = PixelFormat::NV12;
  out.width = w; out.height = h;
  out.stride.s0 = w; out.stride.s1 = w;
  out.plane0 = static_cast<uint8_t*>(y);
  out.plane1 = static_cast<uint8_t*>(uv);
  out.bytes0 = static_cast<size_t>(h) * w;
  out.bytes1 = static_cast<size_t>(h/2) * w;
  out.pool_cookie = nullptr; // optional; your RM can fill it
  out.pinned = y_pool.pinned() && uv_pool.pinned();
  return true;
}

#endif // MEM_POOL_H

