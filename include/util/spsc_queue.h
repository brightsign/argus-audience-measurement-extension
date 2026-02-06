#ifndef SPSC_QUEUE_H
#define SPSC_QUEUE_H

#include <atomic>
#include <array>
#include <cstddef>

/**
 * Single-Producer Single-Consumer lock-free queue (ring buffer)
 * 
 * Thread-safe for one producer thread and one consumer thread.
 * Uses atomic operations for synchronization without locks.
 * 
 * Usage:
 *   SPSCQueue<FrameData, 8> queue;
 *   
 *   // Producer thread:
 *   if (queue.try_enqueue(data)) { ... }
 *   
 *   // Consumer thread:
 *   FrameData data;
 *   if (queue.try_dequeue(data)) { ... }
 */
template<typename T, size_t Capacity>
class SPSCQueue {
public:
  static_assert(Capacity > 0, "Queue capacity must be greater than 0");
  static_assert((Capacity & (Capacity - 1)) == 0, "Queue capacity must be power of 2");
  
  SPSCQueue() noexcept : head_(0), tail_(0) {}
  
  // Non-copyable, non-movable (contains atomics)
  SPSCQueue(const SPSCQueue&) = delete;
  SPSCQueue& operator=(const SPSCQueue&) = delete;
  
  /**
   * Try to enqueue an item (producer thread only)
   * Returns true if successful, false if queue is full
   */
  bool try_enqueue(const T& item) noexcept {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t next_head = (head + 1) & mask_;
    
    // Check if queue is full
    if (next_head == tail_.load(std::memory_order_acquire)) {
      return false;
    }
    
    // Write item and advance head
    buffer_[head] = item;
    head_.store(next_head, std::memory_order_release);
    return true;
  }
  
  /**
   * Try to enqueue an item (move semantics, producer thread only)
   * Returns true if successful, false if queue is full
   */
  bool try_enqueue(T&& item) noexcept {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t next_head = (head + 1) & mask_;
    
    // Check if queue is full
    if (next_head == tail_.load(std::memory_order_acquire)) {
      return false;
    }
    
    // Write item and advance head
    buffer_[head] = std::move(item);
    head_.store(next_head, std::memory_order_release);
    return true;
  }
  
  /**
   * Try to dequeue an item (consumer thread only)
   * Returns true if successful, false if queue is empty
   */
  bool try_dequeue(T& item) noexcept {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    
    // Check if queue is empty
    if (tail == head_.load(std::memory_order_acquire)) {
      return false;
    }
    
    // Read item and advance tail
    item = std::move(buffer_[tail]);
    tail_.store((tail + 1) & mask_, std::memory_order_release);
    return true;
  }
  
  /**
   * Check if queue is empty (approximate, may be stale)
   * Safe to call from any thread
   */
  bool empty() const noexcept {
    return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
  }
  
  /**
   * Check if queue is full (approximate, may be stale)
   * Safe to call from any thread
   */
  bool full() const noexcept {
    const size_t head = head_.load(std::memory_order_acquire);
    const size_t tail = tail_.load(std::memory_order_acquire);
    return ((head + 1) & mask_) == tail;
  }
  
  /**
   * Get approximate size (may be stale)
   * Safe to call from any thread
   */
  size_t size() const noexcept {
    const size_t head = head_.load(std::memory_order_acquire);
    const size_t tail = tail_.load(std::memory_order_acquire);
    return (head - tail) & mask_;
  }
  
  /**
   * Get maximum capacity
   */
  constexpr size_t capacity() const noexcept {
    return Capacity - 1;  // Ring buffer loses one slot
  }

private:
  static constexpr size_t mask_ = Capacity - 1;
  
  // Align to cache line to prevent false sharing
  alignas(64) std::atomic<size_t> head_;  // Producer writes here
  alignas(64) std::atomic<size_t> tail_;  // Consumer writes here
  std::array<T, Capacity> buffer_;
};

#endif // SPSC_QUEUE_H
