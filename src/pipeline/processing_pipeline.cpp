#include "pipeline/processing_pipeline.h"

bool ProcessingPipeline::build() noexcept {
  rm_ = ResourceManager::create(cfg_.res_cfg);
  if (!rm_->init_rga()) return false;
  if (!rm_->init_scratch_pools()) return false;

  input_   = make_input(cfg_.input);
  if (!input_) return false;

  capture_ = std::make_unique<CaptureWorker>(std::move(input_), &qA_);
  pre_     = std::make_unique<PreprocessStage>(rm_.get(), &cfg_.model, cfg_.pre_cfg, &qA_, &qB_);
  runner_  = make_model_runner(cfg_.model);
  infer_   = std::make_unique<InferenceStage>(std::move(runner_), &qB_, /*out_q*/nullptr);
  post_    = std::make_unique<PostprocessStage>(&cfg_.proc, /*in_q*/nullptr, cb_);
  // NOTE: We didn't wire the last two queues in this stub (kept minimal). Fill when you add Queue C.
  return true;
}

void ProcessingPipeline::teardown() noexcept {
  if (post_)   post_->stop();
  if (infer_)  infer_->stop();
  if (pre_)    pre_->stop();
  if (capture_) capture_->stop();
  post_.reset(); infer_.reset(); pre_.reset(); capture_.reset();
  rm_.reset();
}

bool ProcessingPipeline::start() noexcept {
  if (!build()) return false;
  if (capture_)  capture_->start();
  if (pre_)      pre_->start();
  if (infer_)    infer_->start();
  if (post_)     post_->start();
  return true;
}
void ProcessingPipeline::stop() noexcept { teardown(); }

