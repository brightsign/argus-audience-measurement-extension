#include <gtest/gtest.h>
#include <fcntl.h>
#include <unistd.h>
#include "input/v4l2_controls.h"

using v4l2ctl::ApplyResult;

// No V4L2 hardware is needed: devices without the control (or that are not V4L2 at all) must be handled gracefully.
TEST(V4l2Controls, NonV4l2FdIsUnsupportedNotFatal) {
    int fd = ::open("/dev/null", O_RDWR);
    ASSERT_GE(fd, 0);
    int before = 7, actual = 7;
    EXPECT_EQ(v4l2ctl::set_exposure_auto_priority(fd, 0, &before, &actual), ApplyResult::Unsupported);
    EXPECT_EQ(before, -1);
    EXPECT_EQ(actual, -1);
    ::close(fd);
}

TEST(V4l2Controls, InvalidArgumentsAreRejectedWithoutTouchingTheDevice) {
    int fd = ::open("/dev/null", O_RDWR);
    ASSERT_GE(fd, 0);
    EXPECT_EQ(v4l2ctl::set_exposure_auto_priority(-1, 0, nullptr, nullptr), ApplyResult::Unsupported);
    EXPECT_EQ(v4l2ctl::set_exposure_auto_priority(fd, 2, nullptr, nullptr), ApplyResult::Unsupported);
    EXPECT_EQ(v4l2ctl::set_exposure_auto_priority(fd, -1, nullptr, nullptr), ApplyResult::Unsupported);
    ::close(fd);
}
