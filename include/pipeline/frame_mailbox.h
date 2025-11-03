#ifndef FRAME_MAILBOX_H
#define FRAME_MAILBOX_H

#include <atomic>
#include <memory>
#include "pipeline/shared_frame.h"

/**
 * FrameMailbox: A 1-deep "latest-wins" mailbox for frame fan-out.
 * 
 * Single-producer, single-consumer pattern (lock-free, GCC 9 safe):
 * - Capture thread posts frames here
 * - Model worker threads consume from individual mailboxes
 * 
 * Uses atomic_load/atomic_store free functions on plain shared_ptr<T>
 * for C++11 compatibility and GCC 9 libstdc++ safety.
 */
class FrameMailbox {
public:
    FrameMailbox() noexcept : has_frame_(false) {}

    /**
     * Post a frame (lock-free, atomic).
     * Capture thread calls this once per captured frame.
     */
    void postFrame(const std::shared_ptr<SharedFrame>& f) noexcept {
        std::atomic_store_explicit(&buffer_, f, std::memory_order_release);
        has_frame_.store(true, std::memory_order_release);
    }

    /**
     * Take a frame if available (lock-free, atomic).
     * Returns nullptr if no new frame since last take.
     * Model thread calls this in a loop with sleep on nullptr.
     */
    std::shared_ptr<SharedFrame> takeFrame() noexcept {
        if (!has_frame_.load(std::memory_order_acquire)) {
            return nullptr;
        }

        // Get and clear atomically
        auto f = std::atomic_exchange_explicit(
            &buffer_, std::shared_ptr<SharedFrame>{}, std::memory_order_acq_rel);
        has_frame_.store(false, std::memory_order_release);
        return f;
    }

private:
    // NOT atomic<T>; we use atomic_* free functions for GCC 9 compat
    std::shared_ptr<SharedFrame> buffer_{nullptr};
    std::atomic<bool> has_frame_;
};

#endif // FRAME_MAILBOX_H
