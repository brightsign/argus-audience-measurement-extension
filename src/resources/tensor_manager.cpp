#include "resources/tensor_manager.h"

struct RknnTensorManager::Impl {
  std::vector<uint8_t> input;
  std::vector<std::vector<uint8_t>> outputs;
  TensorDesc in_desc{};
  std::vector<TensorDesc> out_descs;
};

std::unique_ptr<RknnTensorManager> RknnTensorManager::create() noexcept {
  auto t = std::unique_ptr<RknnTensorManager>(new RknnTensorManager());
  t->p_.reset(new Impl());
  return t;
}
RknnTensorManager::~RknnTensorManager() = default;

bool RknnTensorManager::init_from_model(const char* model_path, const TensorDesc& input, const std::vector<TensorDesc>& outputs) noexcept {
  (void)model_path;
  if (!p_) return false;
  p_->in_desc = input; p_->out_descs = outputs;
  size_t in_bytes = static_cast<size_t>(input.n) * (input.nhwc ? (input.h*input.w*input.c) : (input.c*input.h*input.w)) * input.elem_bytes;
  p_->input.resize(in_bytes);
  p_->outputs.resize(outputs.size());
  for (size_t i=0;i<outputs.size();++i) {
    const auto& d = outputs[i];
    size_t bytes = static_cast<size_t>(d.n) * (d.nhwc ? (d.h*d.w*d.c) : (d.c*d.h*d.w)) * d.elem_bytes;
    p_->outputs[i].resize(bytes);
  }
  return true;
}

uint8_t* RknnTensorManager::input_ptr() noexcept { return p_ ? p_->input.data() : nullptr; }
uint8_t* RknnTensorManager::output_ptr(int index) noexcept {
  if (!p_) return nullptr; if (index<0 || static_cast<size_t>(index)>=p_->outputs.size()) return nullptr;
  return p_->outputs[static_cast<size_t>(index)].data();
}
int RknnTensorManager::num_outputs() const noexcept { return p_ ? static_cast<int>(p_->outputs.size()) : 0; }
bool RknnTensorManager::reshape_input(int new_w, int new_h) noexcept {
  if (!p_) return false; p_->in_desc.w=new_w; p_->in_desc.h=new_h; return true;
}
void RknnTensorManager::unload() noexcept { if (p_) { p_->input.clear(); p_->outputs.clear(); } }

