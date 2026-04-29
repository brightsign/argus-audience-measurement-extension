#pragma once
#include <string>
#include <memory>
#include <cstdint>

/**
 * MobileNetV3Classifier
 *
 * Lightweight RKNN-backed INT8 classifier for the employee vest binary task.
 *
 * Class indices (must match training order):
 *   0 = no_vest
 *   1 = employee_vest
 *
 * Input:  raw uint8 RGB 224×224 (3-channel, HWC, packed).
 *         Normalisation (ImageNet mean/std) is baked into the RKNN graph
 *         by the conversion script — no extra pre-processing needed here.
 *
 * Output: float probabilities[N_CLASSES] after softmax.
 */
class MobileNetV3Classifier {
public:
    static constexpr int INPUT_W   = 224;
    static constexpr int INPUT_H   = 224;
    static constexpr int N_CLASSES = 2;

    // Class label helpers
    static constexpr int CLASS_NO_VEST      = 0;
    static constexpr int CLASS_EMPLOYEE_VEST = 1;

    MobileNetV3Classifier() noexcept;
    ~MobileNetV3Classifier();

    // Non-copyable
    MobileNetV3Classifier(const MobileNetV3Classifier&) = delete;
    MobileNetV3Classifier& operator=(const MobileNetV3Classifier&) = delete;

    /**
     * Load an RKNN model file.
     * @param model_path  Path to Mobilenetv3_small.rknn
     * @param npu_core    0/1/2 = pin to specific NPU core; -1 = auto
     * @return true on success
     */
    bool load(const std::string& model_path, int npu_core = 2) noexcept;

    /** Release RKNN resources. Safe to call even if not loaded. */
    void unload() noexcept;

    bool is_loaded() const noexcept;

    /**
     * Run one classification on a 224×224 RGB crop.
     * @param rgb_hwc  Raw uint8 RGB, packed HWC, exactly INPUT_H*INPUT_W*3 bytes
     * @param probs    Output: N_CLASSES softmax probabilities (caller-allocated)
     * @return true on success; false leaves probs unchanged
     */
    bool classify(const uint8_t* rgb_hwc, float probs[N_CLASSES]) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
