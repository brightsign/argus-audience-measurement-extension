#include <gtest/gtest.h>

TEST(SimpleTest, BasicAssertion) {
    EXPECT_EQ(1 + 1, 2);
    EXPECT_TRUE(true);
}

TEST(SimpleTest, StringTest) {
    std::string hello = "Hello";
    EXPECT_EQ(hello, "Hello");
}
