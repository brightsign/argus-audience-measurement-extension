#include "models/model_runner_mobilenetv3.h"
#include "metrics/log_global.h"

extern "C" {
#include "rknn_api.h"
#include "file_utils.h"
}

#include <cmath>
#include <cstring>
#include <vector>

// ============================================================================
// Internal implementation
// ============================================================================

struct MobileNetV3Classifier::Impl {
    rknn_context ctx{0};
    bool          loaded{false};

    // Pre-allocated input buffer (INPUT_H × INPUT_W × 3 bytes, RGB uint8)
    static constexpr int BUF_BYTES = MobileNetV3Classifier::INPUT_H
                                   * MobileNetV3Classifier::INPUT_W * 3;
    uint8_t in_buf[BUF_BYTES]{};
};

// ============================================================================
// Public interface
// ============================================================================

MobileNetV3Classifier::MobileNetV3Classifier() noexcept
    : p_(std::make_unique<Impl>()) {}

MobileNetV3Classifier::~MobileNetV3Classifier() {
    unload();
}

bool MobileNetV3Classifier::is_loaded() const noexcept {
    return p_ && p_->loaded;
}

bool MobileNetV3Classifier::load(const std::string& model_path, int npu_core) noexcept {
    if (!p_) return false;

    if (model_path.empty()) {
        LG_ERROR("MobileNetV3: empty model_path");
        return false;
    }

    // ---------- read model file ----------
    int model_len = 0;
    char* model_data = nullptr;
    model_len = read_data_from_file(model_path.c_str(), &model_data);
    if (!model_data || model_len <= 0) {
        LG_ERROR("MobileNetV3: failed to read model file: %s", model_path.c_str());
        return false;
    }

    // ---------- init RKNN context ----------
    int ret = rknn_init(&p_->ctx, model_data, model_len, 0, nullptr);
    free(model_data);
    if (ret < 0) {
        LG_ERROR("MobileNetV3: rknn_init failed ret=%d path=%s", ret, model_path.c_str());
        return false;
    }

    // ---------- pin to NPU core ----------
    if (npu_core >= 0) {
        rknn_core_mask core_mask;
        switch (npu_core) {
            case 0:  core_mask = RKNN_NPU_CORE_0;     break;
            case 1:  core_mask = RKNN_NPU_CORE_1;     break;
            case 2:  core_mask = RKNN_NPU_CORE_2;     break;
            default: core_mask = RKNN_NPU_CORE_0_1_2; break;
        }
        ret = rknn_set_core_mask(p_->ctx, core_mask);
        if (ret != RKNN_SUCC) {
            LG_WARN("MobileNetV3: rknn_set_core_mask(core=%d) failed ret=%d", npu_core, ret);
        } else {
            LG_INFO("MobileNetV3: pinned to NPU core %d", npu_core);
        }
    }

    // ---------- verify I/O layout ----------
    rknn_input_output_num io_num{};
    ret = rknn_query(p_->ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC || io_num.n_input < 1 || io_num.n_output < 1) {
        LG_ERROR("MobileNetV3: rknn_query io_num failed ret=%d in=%u out=%u",
                 ret, io_num.n_input, io_num.n_output);
        rknn_destroy(p_->ctx);
        return false;
    }
    LG_INFO("MobileNetV3: n_input=%u n_output=%u", io_num.n_input, io_num.n_output);

    // ---------- self-test with black frame ----------
    {
        std::memset(p_->in_buf, 0, Impl::BUF_BYTES);

        rknn_input inputs[1]{};
        inputs[0].index        = 0;
        inputs[0].type         = RKNN_TENSOR_UINT8;
        inputs[0].fmt          = RKNN_TENSOR_NHWC;
        inputs[0].size         = Impl::BUF_BYTES;
        inputs[0].buf          = p_->in_buf;
        inputs[0].pass_through = 0;

        ret = rknn_inputs_set(p_->ctx, 1, inputs);
        if (ret < 0) {
            LG_ERROR("MobileNetV3: self-test rknn_inputs_set failed ret=%d", ret);
            rknn_destroy(p_->ctx);
            return false;
        }

        ret = rknn_run(p_->ctx, nullptr);
        if (ret < 0) {
            LG_ERROR("MobileNetV3: self-test rknn_run failed ret=%d", ret);
            rknn_destroy(p_->ctx);
            return false;
        }

        // Read output (ignore result; just confirm it runs)
        float out_buf[N_CLASSES]{};
        rknn_output outputs[1]{};
        outputs[0].index     = 0;
        outputs[0].want_float = 1;
        outputs[0].is_prealloc = 1;
        outputs[0].buf       = out_buf;
        outputs[0].size      = sizeof(out_buf);

        ret = rknn_outputs_get(p_->ctx, 1, outputs, nullptr);
        if (ret < 0) {
            LG_ERROR("MobileNetV3: self-test rknn_outputs_get failed ret=%d", ret);
            rknn_outputs_release(p_->ctx, 1, outputs);
            rknn_destroy(p_->ctx);
            return false;
        }
        rknn_outputs_release(p_->ctx, 1, outputs);
        LG_INFO("MobileNetV3: self-test OK (logits=[%.3f %.3f])", out_buf[0], out_buf[1]);
    }

    p_->loaded = true;
    LG_INFO("MobileNetV3: loaded %s", model_path.c_str());
    return true;
}

void MobileNetV3Classifier::unload() noexcept {
    if (!p_ || !p_->loaded) return;
    rknn_destroy(p_->ctx);
    p_->ctx    = 0;
    p_->loaded = false;
    LG_INFO("MobileNetV3: unloaded");
}

bool MobileNetV3Classifier::classify(const uint8_t* rgb_hwc,
                                     float probs[N_CLASSES]) noexcept {
    if (!p_ || !p_->loaded) return false;
    if (!rgb_hwc || !probs)  return false;

    // ---------- set input ----------
    std::memcpy(p_->in_buf, rgb_hwc, Impl::BUF_BYTES);

    rknn_input inputs[1]{};
    inputs[0].index        = 0;
    inputs[0].type         = RKNN_TENSOR_UINT8;
    inputs[0].fmt          = RKNN_TENSOR_NHWC;
    inputs[0].size         = Impl::BUF_BYTES;
    inputs[0].buf          = p_->in_buf;
    inputs[0].pass_through = 0;

    int ret = rknn_inputs_set(p_->ctx, 1, inputs);
    if (ret < 0) {
        LG_ERROR("MobileNetV3::classify: rknn_inputs_set failed ret=%d", ret);
        return false;
    }

    // ---------- run ----------
    ret = rknn_run(p_->ctx, nullptr);
    if (ret < 0) {
        LG_ERROR("MobileNetV3::classify: rknn_run failed ret=%d", ret);
        return false;
    }

    // ---------- get output ----------
    float logits[N_CLASSES]{};
    rknn_output outputs[1]{};
    outputs[0].index       = 0;
    outputs[0].want_float  = 1;
    outputs[0].is_prealloc = 1;
    outputs[0].buf         = logits;
    outputs[0].size        = sizeof(logits);

    ret = rknn_outputs_get(p_->ctx, 1, outputs, nullptr);
    if (ret < 0) {
        LG_ERROR("MobileNetV3::classify: rknn_outputs_get failed ret=%d", ret);
        rknn_outputs_release(p_->ctx, 1, outputs);
        return false;
    }
    rknn_outputs_release(p_->ctx, 1, outputs);

    // ---------- softmax ----------
    float max_l = logits[0];
    for (int i = 1; i < N_CLASSES; ++i)
        if (logits[i] > max_l) max_l = logits[i];

    float sum = 0.0f;
    for (int i = 0; i < N_CLASSES; ++i) {
        probs[i] = std::exp(logits[i] - max_l);
        sum += probs[i];
    }
    for (int i = 0; i < N_CLASSES; ++i)
        probs[i] /= sum;

    return true;
}
