#include "models/model_factory.h"
#include "models/model_runner_retinaface.h"
#include "models/model_runner_yolox.h"
#include <algorithm>

std::unique_ptr<IModelRunner> make_model_runner(const ModelSpec& spec) {
  // Prefer explicit family; fall back to name contains
  if (spec.family == ModelFamily::RetinaFace) {
    return std::make_unique<RKNNRetinafaceRunner>();
  }
  
  if (spec.family == ModelFamily::YOLOX) {
    return std::make_unique<RKNNYoloXRunner>();
  }

  // Fallback: check name for model type
  std::string name = spec.name;
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c){ return std::tolower(c); });
  
  if (name.find("retinaface") != std::string::npos) {
    return std::make_unique<RKNNRetinafaceRunner>();
  }
  
  if (name.find("yolox") != std::string::npos) {
    return std::make_unique<RKNNYoloXRunner>();
  }

  return nullptr;
}
