#include <gtest/gtest.h>

#include "hello_cross_platform/platform.h"

TEST(HelloCoreTest, BuildMessageNotEmpty) {
    const std::string message = hello_cross_platform::build_message();
    EXPECT_FALSE(message.empty());
}

TEST(HelloCoreTest, DetectPlatformValid) {
    const std::string platform = hello_cross_platform::detect_platform();
    EXPECT_FALSE(platform.empty());
    EXPECT_NE(platform, "Unknown");
}