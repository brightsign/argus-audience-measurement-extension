// SPDX-License-Identifier: MIT
// pp1 preprocessing: shape, constant-image closed form, BGR/RGB equivalence, invalid inputs.
#include "visualmemory/preprocess.h"
#include "test_util.h"

#include <gtest/gtest.h>
#include <cmath>

using namespace visualmemory;

TEST(Preprocess, ConstantImageMatchesClosedForm) {
    const int w = 97, h = 53;   // odd sizes exercise the filter edges
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0; i < px.size(); i += 3) { px[i] = 200; px[i + 1] = 100; px[i + 2] = 50; }   // R,G,B
    ImageView v; v.data = px.data(); v.width = w; v.height = h; v.format = PixelFormat::RGB24;
    Pp1Preprocessor pp; std::vector<float> out(kPp1Floats); std::string err;
    ASSERT_TRUE(pp.run(v, out.data(), err)) << err;
    const float mean[3] = {0.485f, 0.456f, 0.406f}, stdv[3] = {0.229f, 0.224f, 0.225f}, c[3] = {200, 100, 50};
    for (size_t i = 0; i < out.size(); ++i)
        ASSERT_NEAR(out[i], (c[i % 3] / 255.f - mean[i % 3]) / stdv[i % 3], 2e-5f) << "i=" << i;  // weights sum to 1
}

TEST(Preprocess, BgrAndRgbGiveIdenticalTensor) {
    auto rgb = vmtest::make_scene_rgb(160, 90, 5);
    std::vector<uint8_t> bgr(rgb.size());
    for (size_t i = 0; i < rgb.size(); i += 3) { bgr[i] = rgb[i + 2]; bgr[i + 1] = rgb[i + 1]; bgr[i + 2] = rgb[i]; }
    ImageView a, b;
    a.data = rgb.data(); a.width = 160; a.height = 90; a.format = PixelFormat::RGB24;
    b.data = bgr.data(); b.width = 160; b.height = 90; b.format = PixelFormat::BGR24;
    Pp1Preprocessor pa, pb; std::vector<float> oa(kPp1Floats), ob(kPp1Floats); std::string err;
    ASSERT_TRUE(pa.run(a, oa.data(), err)); ASSERT_TRUE(pb.run(b, ob.data(), err));
    EXPECT_EQ(oa, ob);
}

TEST(Preprocess, StridedInputEqualsPackedInput) {
    const int w = 64, h = 48, stride = w * 3 + 17;
    auto packed = vmtest::make_scene_rgb(w, h, 9);
    std::vector<uint8_t> strided(static_cast<size_t>(stride) * h, 0xAB);
    for (int y = 0; y < h; ++y) std::copy_n(packed.begin() + y * w * 3, w * 3, strided.begin() + y * stride);
    ImageView a, b; a.data = packed.data(); a.width = w; a.height = h;
    b.data = strided.data(); b.width = w; b.height = h; b.stride = stride;
    Pp1Preprocessor pa, pb; std::vector<float> oa(kPp1Floats), ob(kPp1Floats); std::string err;
    ASSERT_TRUE(pa.run(a, oa.data(), err)); ASSERT_TRUE(pb.run(b, ob.data(), err));
    EXPECT_EQ(oa, ob);
}

TEST(Preprocess, UpscaleAndRepeatedCallsAreDeterministic) {
    auto px = vmtest::make_scene_rgb(32, 20, 3);   // smaller than 224: upscale path
    ImageView v; v.data = px.data(); v.width = 32; v.height = 20;
    Pp1Preprocessor pp; std::vector<float> a(kPp1Floats), b(kPp1Floats); std::string err;
    ASSERT_TRUE(pp.run(v, a.data(), err)); ASSERT_TRUE(pp.run(v, b.data(), err));
    EXPECT_EQ(a, b);
    for (float x : a) ASSERT_TRUE(std::isfinite(x));
}

TEST(Preprocess, RejectsInvalidInput) {
    Pp1Preprocessor pp; std::vector<float> out(kPp1Floats); std::string err;
    ImageView v;
    EXPECT_FALSE(pp.run(v, out.data(), err));                       // null data
    uint8_t px[12] = {0};
    v.data = px; v.width = 1; v.height = 4;
    EXPECT_FALSE(pp.run(v, out.data(), err));                       // width < 2
    v.width = 2; v.height = 2; v.stride = 3;
    EXPECT_FALSE(pp.run(v, out.data(), err));                       // stride < row
    v.stride = 0; v.format = PixelFormat::NV12;
    EXPECT_FALSE(pp.run(v, out.data(), err));                       // unsupported format
    v.format = PixelFormat::RGB24;
    EXPECT_FALSE(pp.run(v, nullptr, err));                          // null output
}
