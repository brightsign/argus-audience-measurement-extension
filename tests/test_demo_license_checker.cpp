#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>

#include "demo/demo_license_checker.h"

namespace {

std::string temp_license_file(const std::string& suffix) {
    return "/tmp/argus_test_license_" + suffix + ".json";
}

void write_file(const std::string& path, const std::string& content) {
    std::ofstream ofs(path);
    ofs << content;
}

void remove_file(const std::string& path) {
    std::remove(path.c_str());
}

// Format a time_t as "YYYY-MM-DDTHH:MM:SSZ".
std::string format_utc(std::time_t t) {
    std::tm utc{};
    gmtime_r(&t, &utc);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buf;
}

std::string valid_future_json() {
    return R"({"version":1,"expires_utc":"2099-12-31T23:59:59Z"})";
}

std::string valid_past_json() {
    return R"({"version":1,"expires_utc":"2000-01-01T00:00:00Z"})";
}

}  // namespace

// --- file absent ---

TEST(DemoLicenseChecker, AbsentFileTreatedAsExpired) {
    const std::string path = temp_license_file("absent");
    remove_file(path);

    DemoLicenseChecker checker(path);
    EXPECT_TRUE(checker.check());
    EXPECT_TRUE(checker.is_expired());
}

// --- valid file, expiry in future ---

TEST(DemoLicenseChecker, FutureExpiryNotExpired) {
    const std::string path = temp_license_file("future");
    write_file(path, valid_future_json());

    DemoLicenseChecker checker(path);
    EXPECT_FALSE(checker.check());
    EXPECT_FALSE(checker.is_expired());
    EXPECT_EQ(checker.expires_utc(), "2099-12-31T23:59:59Z");

    remove_file(path);
}

// --- valid file, expiry in past ---

TEST(DemoLicenseChecker, PastExpiryIsExpired) {
    const std::string path = temp_license_file("past");
    write_file(path, valid_past_json());

    DemoLicenseChecker checker(path);
    EXPECT_TRUE(checker.check());
    EXPECT_TRUE(checker.is_expired());
    EXPECT_EQ(checker.expires_utc(), "2000-01-01T00:00:00Z");

    remove_file(path);
}

// --- wrong version field ---

TEST(DemoLicenseChecker, WrongVersionTreatedAsExpired) {
    const std::string path = temp_license_file("bad_version");
    write_file(path, R"({"version":99,"expires_utc":"2099-12-31T23:59:59Z"})");

    DemoLicenseChecker checker(path);
    EXPECT_TRUE(checker.check());
    EXPECT_TRUE(checker.is_expired());

    remove_file(path);
}

// --- missing version field ---

TEST(DemoLicenseChecker, MissingVersionTreatedAsExpired) {
    const std::string path = temp_license_file("no_version");
    write_file(path, R"({"expires_utc":"2099-12-31T23:59:59Z"})");

    DemoLicenseChecker checker(path);
    EXPECT_TRUE(checker.check());
    EXPECT_TRUE(checker.is_expired());

    remove_file(path);
}

// --- missing expires_utc field ---

TEST(DemoLicenseChecker, MissingExpiresUtcTreatedAsExpired) {
    const std::string path = temp_license_file("no_expiry");
    write_file(path, R"({"version":1,"note":"no expiry field"})");

    DemoLicenseChecker checker(path);
    EXPECT_TRUE(checker.check());
    EXPECT_TRUE(checker.is_expired());

    remove_file(path);
}

// --- malformed expires_utc ---

TEST(DemoLicenseChecker, MalformedExpiresUtcTreatedAsExpired) {
    const std::string path = temp_license_file("bad_expiry");
    write_file(path, R"({"version":1,"expires_utc":"not-a-date"})");

    DemoLicenseChecker checker(path);
    EXPECT_TRUE(checker.check());
    EXPECT_TRUE(checker.is_expired());

    remove_file(path);
}

// --- expires_utc missing 'Z' suffix ---

TEST(DemoLicenseChecker, ExpiresUtcWithoutZSuffixTreatedAsExpired) {
    const std::string path = temp_license_file("no_z_suffix");
    write_file(path, R"({"version":1,"expires_utc":"2099-12-31T23:59:59"})");

    DemoLicenseChecker checker(path);
    EXPECT_TRUE(checker.check());
    EXPECT_TRUE(checker.is_expired());

    remove_file(path);
}

// --- malformed JSON ---

TEST(DemoLicenseChecker, MalformedJsonTreatedAsExpired) {
    const std::string path = temp_license_file("corrupt_json");
    write_file(path, "this is {{{ not valid json");

    DemoLicenseChecker checker(path);
    EXPECT_TRUE(checker.check());
    EXPECT_TRUE(checker.is_expired());

    remove_file(path);
}

// --- expiry in the past by 1 second ---

TEST(DemoLicenseChecker, ExpiryOneSecondAgoIsExpired) {
    const std::string path = temp_license_file("one_sec_ago");
    std::time_t one_sec_ago = std::time(nullptr) - 1;
    std::string ts = format_utc(one_sec_ago);
    write_file(path, R"({"version":1,"expires_utc":")" + ts + R"("})");

    DemoLicenseChecker checker(path);
    EXPECT_TRUE(checker.check());
    EXPECT_TRUE(checker.is_expired());
    EXPECT_EQ(checker.expires_utc(), ts);

    remove_file(path);
}

// --- expiry far in the future ---

TEST(DemoLicenseChecker, ExpiryFarFutureNotExpired) {
    const std::string path = temp_license_file("far_future");
    std::time_t far_future = std::time(nullptr) + 86400 * 365;  // ~1 year out
    std::string ts = format_utc(far_future);
    write_file(path, R"({"version":1,"expires_utc":")" + ts + R"("})");

    DemoLicenseChecker checker(path);
    EXPECT_FALSE(checker.check());
    EXPECT_EQ(checker.expires_utc(), ts);

    remove_file(path);
}

// --- once expired, stays expired ---

TEST(DemoLicenseChecker, OnceExpiredAlwaysExpired) {
    const std::string path = temp_license_file("sticky_expiry");
    write_file(path, valid_past_json());

    DemoLicenseChecker checker(path);
    EXPECT_TRUE(checker.check());
    EXPECT_TRUE(checker.is_expired());

    // Replace file with a future expiry — checker should still report expired.
    write_file(path, valid_future_json());
    EXPECT_TRUE(checker.check());
    EXPECT_TRUE(checker.is_expired());

    remove_file(path);
}

// --- unreadable file treated as expired (skipped when running as root) ---

TEST(DemoLicenseChecker, UnreadableFileTreatedAsExpired) {
    if (geteuid() == 0) {
        GTEST_SKIP() << "Cannot test unreadable files as root";
    }

    const std::string path = temp_license_file("unreadable");
    write_file(path, valid_future_json());
    chmod(path.c_str(), 0000);

    DemoLicenseChecker checker(path);
    EXPECT_TRUE(checker.check());
    EXPECT_TRUE(checker.is_expired());

    chmod(path.c_str(), 0644);
    remove_file(path);
}

// --- optional fields (issued_utc, note) do not affect result ---

TEST(DemoLicenseChecker, OptionalFieldsIgnored) {
    const std::string path = temp_license_file("optional_fields");
    write_file(path, R"({
        "version": 1,
        "expires_utc": "2099-12-31T23:59:59Z",
        "issued_utc": "2026-01-01T00:00:00Z",
        "note": "test license"
    })");

    DemoLicenseChecker checker(path);
    EXPECT_FALSE(checker.check());
    EXPECT_EQ(checker.expires_utc(), "2099-12-31T23:59:59Z");

    remove_file(path);
}
