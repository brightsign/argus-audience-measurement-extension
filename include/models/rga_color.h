#pragma once

#include <cstdint>
#include <cstddef>

#include <rga/rga.h>
#include <rga/im2d.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace model_pre {

// Convert a contiguous (tightly-packed) BGR888 image to a contiguous RGB888
// image of the same WxH.
//
// Offloads the channel swap to the RGA hardware block (near-zero CPU cost),
// falling back to SIMD cv::cvtColor if RGA is unavailable or fails. This
// replaces a scalar per-pixel swap loop that cost ~8ms/frame on RK3568.
//
// Both src and dst must point to buffers of at least w*h*3 bytes.
inline void bgr_to_rgb_packed(const uint8_t* src, uint8_t* dst, int w, int h) noexcept {
    rga_buffer_t s = wrapbuffer_virtualaddr(const_cast<uint8_t*>(src), w, h, RK_FORMAT_BGR_888);
    rga_buffer_t d = wrapbuffer_virtualaddr(dst, w, h, RK_FORMAT_RGB_888);
    if (imcvtcolor(s, d, RK_FORMAT_BGR_888, RK_FORMAT_RGB_888, IM_SYNC) == IM_STATUS_SUCCESS) {
        return;
    }
    // Fallback: SIMD-accelerated color swap on the CPU.
    cv::Mat src_mat(h, w, CV_8UC3, const_cast<uint8_t*>(src));
    cv::Mat dst_mat(h, w, CV_8UC3, dst);
    cv::cvtColor(src_mat, dst_mat, cv::COLOR_BGR2RGB);
}

} // namespace model_pre
