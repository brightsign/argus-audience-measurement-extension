#ifndef ANNOTATOR_H
#define ANNOTATOR_H

#include <memory>
#include "output_types.h"

// Forward decls to avoid heavy deps:
struct PipelineResult;
struct ImageBuffer;   // from resource_types.h
struct FrameView;     // from resource_types.h or pipeline_types.h

// Interface to draw boxes/landmarks/gaze onto a frame (RGB or NV12 target).
// HW-first (RGA) or SW-fallback is hidden in Impl.
class IFrameAnnotator {
public:
  virtual ~IFrameAnnotator() = default;

  // Draw onto 'dst' using info from 'result' (matching resolution).
  // Returns false on failure. No allocations required.
  virtual bool annotate(const PipelineResult& result,
                        ImageBuffer& dst,
                        const AnnotationSpec& style) noexcept = 0;
};

std::unique_ptr<IFrameAnnotator> make_annotator_hw_first() noexcept;

#endif // ANNOTATOR_H

