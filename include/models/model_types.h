#ifndef MODEL_TYPES_H
#define MODEL_TYPES_H

#include <cstdint>

enum class ModelFamily : uint8_t {
  Unknown = 0,
  RetinaFace,
  YOLOX,
  Custom
};

enum class Backend : uint8_t {
  RKNN = 1,
  CPU  = 2
};

enum class TaskType : uint8_t {
  Detector   = 1,
  Classifier = 2,
  Keypoint   = 3
};

#endif // MODEL_TYPES_H
