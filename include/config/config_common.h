#ifndef CONFIG_COMMON_H
#define CONFIG_COMMON_H

#include <cstdint>
#include <string>
#include <vector>

enum class PixelFormat : uint8_t { NV12, RGB24, BGR24, GRAY8, UNKNOWN };
//enum class ChannelOrder : uint8_t { CHW, HWC };
enum class Normalize : uint8_t { None, MeanStd, Range01, RangeM11 };

enum class NmsMethod : uint8_t { Greedy, Soft };
enum class PublisherKind : uint8_t { UDP, File, Mqtt, Stdout, None };

struct Size2i { int w{0}; int h{0}; };

#endif // CONFIG_COMMON_H

