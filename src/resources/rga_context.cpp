#include "resources/rga_context.h"
#include <chrono>

struct RgaContext::Impl {
  bool hw{false};
  int64_t t_conv{0}, t_resize{0}, t_lb{0};
};

std::unique_ptr<RgaContext> RgaContext::create() noexcept {
  auto ctx = std::unique_ptr<RgaContext>(new RgaContext());
  ctx->p_.reset(new Impl{false});
  return ctx;
}
RgaContext::~RgaContext() = default;

bool RgaContext::hw_available() const noexcept { return p_ && p_->hw; }
bool RgaContext::supports_nv12_to_rgb() const noexcept { return true; }
bool RgaContext::supports_resize() const noexcept { return true; }

OpResult RgaContext::nv12_to_rgb(const FrameView& in_nv12, ImageBuffer& out_rgb) noexcept {
  (void)in_nv12; (void)out_rgb;
  // TODO: implement HW path; for now, no-op
  return OpResult::FallbackUsed;
}
OpResult RgaContext::resize(const FrameView& in, ImageBuffer& out) noexcept {
  (void)in; (void)out;
  return OpResult::FallbackUsed;
}
OpResult RgaContext::letterbox(const FrameView& in, ImageBuffer& out, Rect2i& used_roi, float& scale) noexcept {
  (void)in; (void)out; used_roi = {}; scale = 1.f;
  return OpResult::FallbackUsed;
}

