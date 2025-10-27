#include "resources/memory_pool.h"
#include <cstdlib>
#include <cstring>

struct FixedBlockPool::Impl { std::vector<void*> all; };

std::unique_ptr<FixedBlockPool> FixedBlockPool::create(const PoolParams& p) noexcept {
  auto pool = std::unique_ptr<FixedBlockPool>(new FixedBlockPool());
  pool->block_bytes_ = p.block_bytes;
  pool->capacity_ = p.blocks;
  pool->alignment_ = p.alignment ? p.alignment : 64;
  pool->pinned_ = p.pinned;
  pool->p_.reset(new Impl);

  pool->free_.resize(p.blocks, nullptr);
  pool->available_.store(p.blocks, std::memory_order_relaxed);

  for (uint32_t i=0;i<p.blocks;++i) {
    void* ptr = nullptr;
    if (posix_memalign(&ptr, pool->alignment_, p.block_bytes) != 0) { ptr = nullptr; }
    if (!ptr) return nullptr;
    pool->p_->all.push_back(ptr);
    pool->free_[i] = ptr;
  }
  pool->top_.store(p.blocks, std::memory_order_release);
  return pool;
}

FixedBlockPool::~FixedBlockPool() {
  if (p_) {
    for (void* ptr : p_->all) { free(ptr); }
  }
}

void* FixedBlockPool::acquire() noexcept {
  uint32_t top = top_.load(std::memory_order_acquire);
  if (top == 0) return nullptr;
  uint32_t new_top = top - 1;
  if (!top_.compare_exchange_strong(top, new_top, std::memory_order_acq_rel)) return nullptr;
  available_.fetch_sub(1, std::memory_order_relaxed);
  return free_[new_top];
}

void FixedBlockPool::release(void* ptr) noexcept {
  if (!ptr) return;
  uint32_t top = top_.load(std::memory_order_acquire);
  if (top >= capacity_) return; // overflow guard
  free_[top] = ptr;
  top_.store(top+1, std::memory_order_release);
  available_.fetch_add(1, std::memory_order_relaxed);
}

