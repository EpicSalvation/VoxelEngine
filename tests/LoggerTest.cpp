#include <gtest/gtest.h>
#include "core/Logger.h"
#include <vector>
#include <string>

struct CapturedMsg {
    Log::Level level;
    std::string category;
    std::string text;
};

class LoggerTest : public ::testing::Test {
protected:
    std::vector<CapturedMsg> captured;

    void SetUp() override {
        Log::setMinLevel(Log::Level::Debug);
        Log::setHandler([this](const Log::Message& m) {
            captured.push_back({m.level, m.category, m.text});
        });
    }

    void TearDown() override {
        Log::setHandler(nullptr);
        Log::setMinLevel(Log::Level::Info);
    }
};

TEST_F(LoggerTest, AllLevelsEmit) {
    Log::debug("d");
    Log::info("i");
    Log::warn("w");
    Log::error("e");
    ASSERT_EQ(captured.size(), 4u);
    EXPECT_EQ(captured[0].level, Log::Level::Debug);
    EXPECT_EQ(captured[1].level, Log::Level::Info);
    EXPECT_EQ(captured[2].level, Log::Level::Warn);
    EXPECT_EQ(captured[3].level, Log::Level::Error);
}

TEST_F(LoggerTest, CategoryTagging) {
    Log::warn("Net", "bad packet");
    ASSERT_EQ(captured.size(), 1u);
    EXPECT_EQ(captured[0].category, "Net");
    EXPECT_EQ(captured[0].text, "bad packet");
}

TEST_F(LoggerTest, MinLevelFilters) {
    Log::setMinLevel(Log::Level::Warn);
    Log::debug("no");
    Log::info("no");
    Log::warn("yes");
    Log::error("yes");
    EXPECT_EQ(captured.size(), 2u);
    EXPECT_EQ(captured[0].level, Log::Level::Warn);
    EXPECT_EQ(captured[1].level, Log::Level::Error);
}

TEST_F(LoggerTest, LegacySetWarnHandlerCompat) {
    Log::setHandler(nullptr);
    std::vector<std::string> legacy;
    Log::setWarnHandler([&legacy](const char* msg) {
        legacy.emplace_back(msg);
    });
    Log::warn("hello");
    Log::info("also captured");
    EXPECT_GE(legacy.size(), 1u);
    EXPECT_EQ(legacy[0], "hello");
    Log::setWarnHandler(nullptr);
}

TEST_F(LoggerTest, UncategorizedHasEmptyCategory) {
    Log::info("plain");
    ASSERT_EQ(captured.size(), 1u);
    EXPECT_EQ(captured[0].category, "");
}
