#ifndef MODEL_FACTORY_H
#define MODEL_FACTORY_H

#include <memory>
#include <vector>
#include <string>
#include "models/model_types.h"
#include "config/model_spec.h"
#include "models/model_runner.h"
#include "models/model_runner_retinaface.h"

std::unique_ptr<IModelRunner> make_model_runner(const ModelSpec& spec);

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
