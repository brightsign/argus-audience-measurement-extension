#ifndef MODEL_FACTORY_H
#define MODEL_FACTORY_H

#include <memory>
#include <vector>
#include <string>
#include "model_types.h"
#include "model_spec.h"
#include "model_runner.h"
#include "model_runner_retinaface.h"

inline std::unique_ptr<IModelRunner> make_model_runner(const ModelSpec& spec) {
  switch (spec.family) {
    case ModelFamily::RetinaFace:
      return std::make_unique<RKNNRetinafaceRunner>();
    // case ModelFamily::YOLOX:
    //   return std::make_unique<RKNNYoloxRunner>();
    default:
      return nullptr;
  }
}

// Convenience: build multiple runners from a list of specs
inline std::vector<std::unique_ptr<IModelRunner>>
make_model_runners(const std::vector<ModelSpec>& specs) {
  std::vector<std::unique_ptr<IModelRunner>> out;
  out.reserve(specs.size());
  for (const auto& s : specs) {
    if (auto r = make_model_runner(s)) {
      if (r->load(s)) out.emplace_back(std::move(r));
    }
  }
  return out;
}

#endif // MODEL_FACTORY_H
