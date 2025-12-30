# FIXME: SOC Auto-Detection for Model Loading

## Problem

Currently, the extension builds separate packages per SOC (RK3568, RK3588, RK3576). The model path is a simple relative path like `model/RetinaFace.rknn` with no runtime detection of which chip is running.

If you deploy an RK3588-compiled model to an RK3568 device, the RKNN runtime fails with error `-1`:

```
retinaface:init_retinaface_model failed (-1) path=/var/volatile/bsext/ext_npu_argus/RK3568/model/RetinaFace.rknn
```

## Solution: Runtime SOC Detection

### 1. Add SOC Detection Function

Add to `src/common/device_info.cpp`:

```cpp
#include <fstream>
#include <string>

namespace device_info {

enum class SocType {
    RK3568,
    RK3588,
    RK3576,
    UNKNOWN
};

SocType detect_soc() noexcept {
    try {
        std::ifstream f("/proc/device-tree/compatible");
        if (!f.is_open()) return SocType::UNKNOWN;

        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());

        if (content.find("rk3588") != std::string::npos) return SocType::RK3588;
        if (content.find("rk3576") != std::string::npos) return SocType::RK3576;
        if (content.find("rk3568") != std::string::npos) return SocType::RK3568;
    } catch (...) {}

    return SocType::UNKNOWN;
}

const char* soc_to_string(SocType soc) noexcept {
    switch (soc) {
        case SocType::RK3568: return "rk3568";
        case SocType::RK3588: return "rk3588";
        case SocType::RK3576: return "rk3576";
        default: return "unknown";
    }
}

} // namespace device_info
```

Add to `include/common/device_info.h`:

```cpp
namespace device_info {

enum class SocType { RK3568, RK3588, RK3576, UNKNOWN };

SocType detect_soc() noexcept;
const char* soc_to_string(SocType soc) noexcept;

} // namespace device_info
```

### 2. Update Model Path Resolution

In `src/main.cpp` or `src/config/configuration.cpp`, modify model path handling:

```cpp
#include "common/device_info.h"

std::string resolve_model_path(const std::string& relative_path) {
    auto soc = device_info::detect_soc();
    const char* soc_str = device_info::soc_to_string(soc);

    // If path is "model/RetinaFace.rknn", transform to "model/rk3588/RetinaFace.rknn"
    // based on detected SOC

    size_t pos = relative_path.find('/');
    if (pos != std::string::npos) {
        std::string base = relative_path.substr(0, pos);      // "model"
        std::string file = relative_path.substr(pos + 1);     // "RetinaFace.rknn"
        return base + "/" + soc_str + "/" + file;
    }

    return relative_path;  // fallback
}
```

### 3. Update Directory Structure

Bundle all SOC models in one package:

```
model/
├── rk3568/
│   ├── RetinaFace.rknn
│   └── yolox_s.rknn
├── rk3588/
│   ├── RetinaFace.rknn
│   └── yolox_s.rknn
└── rk3576/
    ├── RetinaFace.rknn
    └── yolox_s.rknn
```

### 4. Update CMakeLists.txt

Modify install rules to copy all SOC models:

```cmake
# Instead of copying just one SOC's models:
# COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/install/${SOC_DIR}/model ...

# Copy all SOC models:
install(DIRECTORY ${CMAKE_SOURCE_DIR}/install/RK3568/model/ DESTINATION model/rk3568)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/install/RK3588/model/ DESTINATION model/rk3588)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/install/RK3576/model/ DESTINATION model/rk3576)
```

### 5. Update Build Scripts

Modify `scripts/runall.sh` to compile models for ALL SOCs:

```bash
# Compile for all targets
for soc in rk3568 rk3588 rk3576; do
    ./scripts/compile_model.sh --target $soc
done
```

## Testing

1. Build the unified package
2. Deploy to different BrightSign players (LS5/RK3568, XT5/RK3588)
3. Check logs to verify correct model is loaded:
   ```
   [INF] Detected SOC: rk3568
   [INF] Loading model: model/rk3568/RetinaFace.rknn
   ```

## Files to Modify

- `include/common/device_info.h` - add SocType enum and function declarations
- `src/common/device_info.cpp` - add detect_soc() implementation
- `src/main.cpp` - call resolve_model_path() before loading models
- `src/config/configuration.cpp` - alternatively, resolve paths during config parsing
- `CMakeLists.txt` - update install rules for multi-SOC model bundling
- `scripts/runall.sh` - compile models for all SOCs
