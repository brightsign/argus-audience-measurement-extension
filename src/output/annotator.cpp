#include "output/annotator.h"

namespace {
class NullAnnotator final : public IFrameAnnotator {
public:
  bool annotate(const PipelineResult&, ImageBuffer&, const AnnotationSpec&) noexcept override {
    return true; // no-op
  }
};
}

std::unique_ptr<IFrameAnnotator> make_annotator_hw_first() noexcept {
  return std::make_unique<NullAnnotator>();
}

