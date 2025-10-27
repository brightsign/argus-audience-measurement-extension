#include "resources/resource_manager.h"

std::unique_ptr<ResourceManager> ResourceManager::create(const ResourceConfig& rcfg) noexcept {
  return std::unique_ptr<ResourceManager>(new ResourceManager(rcfg));
}
ResourceManager::~ResourceManager() = default;

bool ResourceManager::init_rga() noexcept {
  rga_ = RgaContext::create();
  return static_cast<bool>(rga_);
}

bool ResourceManager::init_scratch_pools() noexcept {
  const int w = cfg_.scratch_nv12.w;
  const int h = cfg_.scratch_nv12.h;
  const uint32_t inflight = cfg_.frames_in_flight ? cfg_.frames_in_flight : 2;

  PoolParams y{ static_cast<size_t>(w*h), inflight, cfg_.alignment, cfg_.pinned };
  PoolParams uv{ static_cast<size_t>(w*(h/2)), inflight, cfg_.alignment, cfg_.pinned };
  PoolParams rgb{ static_cast<size_t>(cfg_.scratch_rgb.w*cfg_.scratch_rgb.h*3), inflight, cfg_.alignment, cfg_.pinned };

  pool_nv12_y_  = FixedBlockPool::create(y);
  pool_nv12_uv_ = FixedBlockPool::create(uv);
  pool_rgb_     = FixedBlockPool::create(rgb);

  return (pool_nv12_y_ && pool_nv12_uv_ && pool_rgb_);
}

bool ResourceManager::init_rknn(const char* model_path, const TensorDesc& input, const std::vector<TensorDesc>& outputs) noexcept {
  rknn_ = RknnTensorManager::create();
  if (!rknn_) return false;
  return rknn_->init_from_model(model_path, input, outputs);
}
void ResourceManager::unload_rknn() noexcept { if (rknn_) rknn_->unload(); }

bool ResourceManager::acquire_nv12_scratch(ImageBuffer& out) noexcept {
  if (!pool_nv12_y_ || !pool_nv12_uv_) return false;
  return acquire_nv12(*pool_nv12_y_, *pool_nv12_uv_, cfg_.scratch_nv12.w, cfg_.scratch_nv12.h, out);
}
bool ResourceManager::acquire_rgb_scratch(ImageBuffer& out) noexcept {
  if (!pool_rgb_) return false;
  void* p = pool_rgb_->acquire();
  if (!p) return false;
  out.fmt = PixelFormat::RGB24;
  out.width = cfg_.scratch_rgb.w; out.height = cfg_.scratch_rgb.h;
  out.stride.s0 = out.width*3; out.stride.s1 = 0;
  out.plane0 = static_cast<uint8_t*>(p);
  out.plane1 = nullptr;
  out.bytes0 = static_cast<size_t>(out.height) * out.stride.s0;
  out.bytes1 = 0;
  out.pool_cookie = pool_rgb_.get();
  out.pinned = pool_rgb_->pinned();
  return true;
}
void ResourceManager::release_scratch(ImageBuffer& buf) noexcept {
  if (buf.fmt == PixelFormat::NV12) {
    if (pool_nv12_y_)  pool_nv12_y_->release(buf.plane0);
    if (pool_nv12_uv_) pool_nv12_uv_->release(buf.plane1);
  } else {
    if (pool_rgb_) pool_rgb_->release(buf.plane0);
  }
  buf = {};
}

void ResourceManager::compute_letterbox_roi(const Size2i& src, const Size2i& dst, Rect2i& roi, float& scale) noexcept {
  float sx = static_cast<float>(dst.w) / src.w;
  float sy = static_cast<float>(dst.h) / src.h;
  scale = (sx < sy) ? sx : sy;
  int w = static_cast<int>(src.w * scale);
  int h = static_cast<int>(src.h * scale);
  roi = { (dst.w - w)/2, (dst.h - h)/2, w, h };
}

