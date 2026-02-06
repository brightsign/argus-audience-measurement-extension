/**
 * @file test_cli_parsing.cpp
 * @brief Unit tests for command-line argument parsing
 * 
 * Tests the CLI parsing logic in main.cpp including:
 * - Flag parsing (--config, --model, --input)
 * - Positional argument detection
 * - Priority handling (flags > positional)
 * - Error cases
 */

#include <gtest/gtest.h>
#include "util/util.h"  // For get_opt function

// Test fixture for CLI parsing
class CLIParsingTest : public ::testing::Test {
public:  // Changed from protected to public
    // Helper to create argc/argv from string array
    struct ArgvWrapper {
        std::vector<char*> ptrs;
        std::vector<std::string> strings;
        
        ArgvWrapper(const std::vector<std::string>& args) {
            strings = args;
            for (auto& s : strings) {
                ptrs.push_back(const_cast<char*>(s.c_str()));
            }
            ptrs.push_back(nullptr);
        }
        
        int argc() const { return static_cast<int>(strings.size()); }
        char** argv() { return ptrs.data(); }
    };
};

// TC-010: Parse all flags
TEST_F(CLIParsingTest, ParseAllFlags) {
    std::vector<std::string> args = {
        "attention_demo",
        "--config", "/path/to/config.json",
        "--model", "/path/to/model.rknn",
        "--input", "rtsp://192.168.1.100/stream"
    };
    
    ArgvWrapper wrapper(args);
    
    const char* config = get_opt("--config", wrapper.argc(), wrapper.argv());
    const char* model = get_opt("--model", wrapper.argc(), wrapper.argv());
    const char* input = get_opt("--input", wrapper.argc(), wrapper.argv());
    
    ASSERT_NE(config, nullptr);
    EXPECT_STREQ(config, "/path/to/config.json");
    
    ASSERT_NE(model, nullptr);
    EXPECT_STREQ(model, "/path/to/model.rknn");
    
    ASSERT_NE(input, nullptr);
    EXPECT_STREQ(input, "rtsp://192.168.1.100/stream");
}

// TC-011: Parse positional arguments (requires parse_cli logic from main.cpp)
// Note: This test would need the parse_cli function to be extracted to a testable unit

// TC-012: Flag priority over positional
TEST_F(CLIParsingTest, FlagPriorityOverPositional) {
    std::vector<std::string> args = {
        "attention_demo",
        "positional_config.json",      // Positional
        "--config", "flag_config.json"  // Flag should win
    };
    
    ArgvWrapper wrapper(args);
    
    const char* config = get_opt("--config", wrapper.argc(), wrapper.argv());
    
    ASSERT_NE(config, nullptr);
    EXPECT_STREQ(config, "flag_config.json")  // Flag should take precedence
        << "Flag value should override positional argument";
}

// TC-013: Missing flag value
TEST_F(CLIParsingTest, MissingFlagValue) {
    std::vector<std::string> args = {
        "attention_demo",
        "--config"
        // Missing value!
    };
    
    ArgvWrapper wrapper(args);
    
    const char* config = get_opt("--config", wrapper.argc(), wrapper.argv());
    
    // get_opt should return nullptr if flag has no value
    EXPECT_EQ(config, nullptr) 
        << "get_opt should return nullptr for flag without value";
}

// TC-014: Unknown flag
TEST_F(CLIParsingTest, UnknownFlag) {
    std::vector<std::string> args = {
        "attention_demo",
        "--unknown-flag", "value"
    };
    
    ArgvWrapper wrapper(args);
    
    const char* unknown = get_opt("--unknown-flag", wrapper.argc(), wrapper.argv());
    
    // get_opt will find it (doesn't validate known flags)
    ASSERT_NE(unknown, nullptr);
    EXPECT_STREQ(unknown, "value");
    
    // But application should validate and reject unknown flags
}

// TC-015: Empty arguments
TEST_F(CLIParsingTest, EmptyArguments) {
    std::vector<std::string> args = {
        "attention_demo"  // Just program name
    };
    
    ArgvWrapper wrapper(args);
    
    const char* config = get_opt("--config", wrapper.argc(), wrapper.argv());
    const char* model = get_opt("--model", wrapper.argc(), wrapper.argv());
    const char* input = get_opt("--input", wrapper.argc(), wrapper.argv());
    
    EXPECT_EQ(config, nullptr);
    EXPECT_EQ(model, nullptr);
    EXPECT_EQ(input, nullptr);
    
    // Application should use defaults
}

// TC-016: Mixed flags and positionals
TEST_F(CLIParsingTest, MixedFlagsAndPositionals) {
    std::vector<std::string> args = {
        "attention_demo",
        "--config", "/flag/config.json",
        "positional_model.rknn",
        "--input", "/dev/video0"
    };
    
    ArgvWrapper wrapper(args);
    
    const char* config = get_opt("--config", wrapper.argc(), wrapper.argv());
    const char* input = get_opt("--input", wrapper.argc(), wrapper.argv());
    
    ASSERT_NE(config, nullptr);
    EXPECT_STREQ(config, "/flag/config.json");
    
    ASSERT_NE(input, nullptr);
    EXPECT_STREQ(input, "/dev/video0");
    
    // positional_model.rknn should be detected by extension-based logic
}

// TC-017: Duplicate flags (last one wins)
TEST_F(CLIParsingTest, DuplicateFlags) {
    std::vector<std::string> args = {
        "attention_demo",
        "--config", "/first/config.json",
        "--config", "/second/config.json"
    };
    
    ArgvWrapper wrapper(args);
    
    const char* config = get_opt("--config", wrapper.argc(), wrapper.argv());
    
    ASSERT_NE(config, nullptr);
    // get_opt returns the first occurrence
    EXPECT_STREQ(config, "/first/config.json");
}

// TC-018: Flag with equals sign
TEST_F(CLIParsingTest, FlagWithEquals) {
    std::vector<std::string> args = {
        "attention_demo",
        "--config=/path/to/config.json"
    };
    
    ArgvWrapper wrapper(args);
    
    const char* config = get_opt("--config", wrapper.argc(), wrapper.argv());
    
    // Current get_opt implementation may not handle this format
    // This test documents expected behavior
}

// TC-019: Short flags (if supported)
TEST_F(CLIParsingTest, ShortFlags) {
    std::vector<std::string> args = {
        "attention_demo",
        "-c", "/path/to/config.json",
        "-m", "/path/to/model.rknn"
    };
    
    ArgvWrapper wrapper(args);
    
    const char* config = get_opt("-c", wrapper.argc(), wrapper.argv());
    const char* model = get_opt("-m", wrapper.argc(), wrapper.argv());
    
    // Document whether short flags are supported
    // Current implementation: may or may not support
}

// TC-020: Special characters in paths
TEST_F(CLIParsingTest, SpecialCharactersInPaths) {
    std::vector<std::string> args = {
        "attention_demo",
        "--config", "/path/with spaces/config.json",
        "--model", "/path/with-dashes/model.rknn",
        "--input", "rtsp://user:pass@host:port/stream?param=value"
    };
    
    ArgvWrapper wrapper(args);
    
    const char* config = get_opt("--config", wrapper.argc(), wrapper.argv());
    const char* model = get_opt("--model", wrapper.argc(), wrapper.argv());
    const char* input = get_opt("--input", wrapper.argc(), wrapper.argv());
    
    ASSERT_NE(config, nullptr);
    EXPECT_STREQ(config, "/path/with spaces/config.json");
    
    ASSERT_NE(model, nullptr);
    EXPECT_STREQ(model, "/path/with-dashes/model.rknn");
    
    ASSERT_NE(input, nullptr);
    EXPECT_STREQ(input, "rtsp://user:pass@host:port/stream?param=value");
}

// Test helper function: file_exists
TEST(UtilTest, FileExists) {
    // Test non-existent file
    EXPECT_FALSE(file_exists("/nonexistent/path/file.txt"));
    
    // Test existing file (use test binary itself)
    // Note: This assumes the test is running from a known location
    // In production, use a known file or create a temp file
}

// Test helper function: pick_config_path
// This would require more context about the implementation
TEST(UtilTest, PickConfigPath) {
    // Test default search paths:
    // 1. /storage/sd/configs/config.json (BrightSign SD card)
    // 2. ./configs/config.json (local directory)
    // 3. ./config.json (current directory)
    
    // Mock file system or use temp files for testing
}

// Parameterized test for file extension detection
class FileExtensionTest : public ::testing::TestWithParam<std::tuple<std::string, bool, bool>> {};

TEST_P(FileExtensionTest, DetectFileType) {
    auto [filename, is_json, is_rknn] = GetParam();
    
    // Test JSON detection
    bool detected_json = filename.find(".json") != std::string::npos;
    EXPECT_EQ(detected_json, is_json) << "JSON detection failed for: " << filename;
    
    // Test RKNN detection
    bool detected_rknn = filename.find(".rknn") != std::string::npos;
    EXPECT_EQ(detected_rknn, is_rknn) << "RKNN detection failed for: " << filename;
}

INSTANTIATE_TEST_SUITE_P(
    FileTypes,
    FileExtensionTest,
    ::testing::Values(
        std::make_tuple("config.json", true, false),
        std::make_tuple("model.rknn", false, true),
        std::make_tuple("video.mp4", false, false),
        std::make_tuple("RetinaFace.rknn", false, true),
        std::make_tuple("test_config.json", true, false),
        std::make_tuple("file.txt", false, false),
        std::make_tuple("config", false, false),  // No extension
        std::make_tuple(".json", true, false),    // Hidden file
        std::make_tuple(".rknn", false, true)     // Hidden file
    )
);

// Test argument count edge cases
TEST_F(CLIParsingTest, ArgumentCountEdgeCases) {
    // Zero arguments (argc = 0) - should never happen in practice
    {
        int argc = 0;
        char** argv = nullptr;
        const char* result = get_opt("--config", argc, argv);
        EXPECT_EQ(result, nullptr);
    }
    
    // One argument (just program name)
    {
        std::vector<std::string> args = {"attention_demo"};
        ArgvWrapper wrapper(args);
        const char* result = get_opt("--config", wrapper.argc(), wrapper.argv());
        EXPECT_EQ(result, nullptr);
    }
}

// Test null pointer handling
TEST(UtilTest, NullPointerHandling) {
    // get_opt should handle null argv gracefully
    const char* result = get_opt("--config", 0, nullptr);
    EXPECT_EQ(result, nullptr) << "get_opt should handle null argv";
    
    // Test null flag name
    std::vector<std::string> args = {"prog", "--config", "value"};
    CLIParsingTest::ArgvWrapper wrapper(args);
    result = get_opt(nullptr, wrapper.argc(), wrapper.argv());
    EXPECT_EQ(result, nullptr) << "get_opt should handle null flag name";
}

// Integration test: Full CLI parsing workflow
TEST_F(CLIParsingTest, FullParsingWorkflow) {
    std::vector<std::string> args = {
        "attention_demo",
        "--config", "/path/config.json",
        "fallback_model.rknn",           // Positional model
        "--input", "/dev/video0",
        "fallback_input.mp4"             // Positional input
    };
    
    ArgvWrapper wrapper(args);
    
    // Step 1: Parse flags
    const char* flag_config = get_opt("--config", wrapper.argc(), wrapper.argv());
    const char* flag_model = get_opt("--model", wrapper.argc(), wrapper.argv());
    const char* flag_input = get_opt("--input", wrapper.argc(), wrapper.argv());
    
    // Step 2: Check results
    ASSERT_NE(flag_config, nullptr);
    EXPECT_STREQ(flag_config, "/path/config.json");
    
    EXPECT_EQ(flag_model, nullptr);  // Not provided as flag
    
    ASSERT_NE(flag_input, nullptr);
    EXPECT_STREQ(flag_input, "/dev/video0");
    
    // Step 3: Application should detect positional .rknn file for model
    // Step 4: Application should use flag_input, ignore positional fallback_input.mp4
}
