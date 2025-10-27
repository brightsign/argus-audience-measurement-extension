#ifndef RESOURCE_TYPES_H
#define RESOURCE_TYPES_H

#include <cstdint>
#include <cstddef>
#include "config/config_common.h"
#include "input/input_source.h"

struct Rect2i { int x{0}, y{0}, w{0}, h{0}; };

struct Strides {
  int s0{0}; // plane 0
  int s1{0}; // plane 1 (NV12 UV)
};


// Owning buffer (from a pool); may be single or bi-planar
struct ImageBuffer {
  PixelFormat fmt{PixelFormat::NV12};
  int width{0}, height{0};
  Strides stride{};
  uint8_t* plane0{nullptr};
  uint8_t* plane1{nullptr};
  // pool bookkeeping
  void*    pool_cookie{nullptr}; // filled by pool
  size_t   bytes0{0}, bytes1{0};
  bool     pinned{false};
};

// Op results (non-throwing)
enum class OpResult : uint8_t { Ok, FallbackUsed, Unsupported, NoMemory, Error };

#endif // RESOURCE_TYPES_H

