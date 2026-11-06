// SPDX-License-Identifier: MIT
// Encoder: round-trip, aspect preservation, and validation rejection.
#include "frame_encoder.h"
#include "test_util.h"

#include <gtest/gtest.h>
#include <turbojpeg.h>

using namespace visualmemory;
using namespace visualmemory::detail;

namespace {

// Decode just the JPEG header to read back dimensions.
bool jpeg_dims(const std::vector<uint8_t>& bytes, int& w, int& h) {
    tjhandle tj = tjInitDecompress();
    if (!tj) return false;
    int subsamp = 0, cs = 0;
    int rc = tjDecompressHeader3(tj, bytes.data(), bytes.size(), &w, &h, &subsamp, &cs);
    tjDestroy(tj);
    return rc == 0;
}

} // namespace

TEST(FrameEncoder, EncodesBgrAndFitsBounds) {
    auto f = vmtest::make_bgr_frame(1280, 720, "cam", 1000, 1);
    EncodedJpeg enc;
    std::string err;
    ASSERT_TRUE(encode_frame(f, 640, 360, 80, enc, err)) << err;
    ASSERT_FALSE(enc.bytes.empty());

    // 1280x720 fit into 640x360 => scale 0.5 => 640x360 exactly.
    EXPECT_EQ(enc.width, 640);
    EXPECT_EQ(enc.height, 360);

    int w = 0, h = 0;
    ASSERT_TRUE(jpeg_dims(enc.bytes, w, h));
    EXPECT_EQ(w, 640);
    EXPECT_EQ(h, 360);

    // JPEG SOI marker.
    EXPECT_EQ(enc.bytes[0], 0xFF);
    EXPECT_EQ(enc.bytes[1], 0xD8);
}

TEST(FrameEncoder, PreservesAspectRatioNoStretch) {
    // 1000x200 (5:1) into 640x360 => limited by width: scale 0.64 => 640x128.
    auto f = vmtest::make_bgr_frame(1000, 200, "cam", 1000, 1);
    EncodedJpeg enc;
    std::string err;
    ASSERT_TRUE(encode_frame(f, 640, 360, 75, enc, err)) << err;
    EXPECT_EQ(enc.width, 640);
    EXPECT_EQ(enc.height, 128);
}

TEST(FrameEncoder, NeverUpscalesSmallFrame) {
    auto f = vmtest::make_bgr_frame(320, 180, "cam", 1000, 1);
    EncodedJpeg enc;
    std::string err;
    ASSERT_TRUE(encode_frame(f, 640, 360, 75, enc, err)) << err;
    EXPECT_EQ(enc.width, 320);
    EXPECT_EQ(enc.height, 180);
}

TEST(FrameEncoder, RejectsBadDimensions) {
    OwnedFrame f;
    f.width = 0;
    f.height = 100;
    f.pixel_format = PixelFormat::BGR24;
    std::string err;
    EXPECT_FALSE(validate_frame(f, err));
}

TEST(FrameEncoder, RejectsTruncatedBuffer) {
    auto f = vmtest::make_bgr_frame(100, 100, "cam", 1, 1);
    f.pixels.resize(f.pixels.size() / 2);  // too small for declared geometry
    std::string err;
    EXPECT_FALSE(validate_frame(f, err));
}

TEST(FrameEncoder, RejectsStrideSmallerThanRow) {
    auto f = vmtest::make_bgr_frame(100, 100, "cam", 1, 1);
    f.stride0 = 100;  // should be >= 300 for BGR24
    std::string err;
    EXPECT_FALSE(validate_frame(f, err));
}

TEST(FrameEncoder, EncodesNv12) {
    OwnedFrame f;
    f.width = 64;
    f.height = 48;
    f.pixel_format = PixelFormat::NV12;
    f.stride0 = 64;
    f.stride1 = 64;
    f.pixels.assign(static_cast<size_t>(64) * 48 + static_cast<size_t>(64) * 24, 128);
    EncodedJpeg enc;
    std::string err;
    ASSERT_TRUE(encode_frame(f, 640, 360, 75, enc, err)) << err;
    int w = 0, h = 0;
    ASSERT_TRUE(jpeg_dims(enc.bytes, w, h));
    EXPECT_EQ(w, 64);
    EXPECT_EQ(h, 48);
}
