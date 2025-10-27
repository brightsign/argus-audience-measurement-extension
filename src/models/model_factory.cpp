#include "models/model_factory.h"
#include "models/model_runner_retinaface.h"
#include <algorithm>

std::unique_ptr<IModelRunner> make_model_runner(const ModelSpec& spec) {
  // Prefer explicit family; fall back to name contains "retinaface"
  if (spec.family == ModelFamily::RetinaFace) {
    return std::make_unique<RKNNRetinafaceRunner>();
  }

  std::string name = spec.name;
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c){ return std::tolower(c); });
  if (name.find("retinaface") != std::string::npos) {
    return std::make_unique<RKNNRetinafaceRunner>();
  }

  // TODO: add other families here (YOLOX, etc.)

  return nullptr;
}
