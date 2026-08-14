// Tests for severity filtering, sink fan-out and the bounded GUI buffer.

#include "cfd/core/Log.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using cfd::LogLevel;
using cfd::LogRecord;
using cfd::Logger;
using cfd::RingBufferSink;

/// The Logger is a process-wide singleton, so each test has to leave it the
/// way it found it. This fixture swaps in a private sink for the duration of
/// a test and restores the previous level afterwards.
class LoggerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    previous_level_ = Logger::instance().level();
    Logger::instance().clearSinks();
    sink_ = std::make_shared<RingBufferSink>(kCapacity);
    Logger::instance().addSink(sink_);
    Logger::instance().setLevel(LogLevel::Trace);
  }

  void TearDown() override {
    Logger::instance().clearSinks();
    Logger::instance().setLevel(previous_level_);
  }

  static constexpr std::size_t kCapacity = 64;
  std::shared_ptr<RingBufferSink> sink_;
  LogLevel previous_level_{LogLevel::Info};
};

TEST_F(LoggerTest, RecordsReachRegisteredSink) {
  CFD_LOG_INFO("test", "chord = {:.2f} m", 1.5);

  const std::vector<LogRecord> records = sink_->snapshot();
  ASSERT_EQ(1u, records.size());
  EXPECT_EQ(LogLevel::Info, records[0].level);
  EXPECT_EQ("test", records[0].category);
  EXPECT_EQ("chord = 1.50 m", records[0].message);
}

TEST_F(LoggerTest, MessagesBelowActiveLevelAreDropped) {
  Logger::instance().setLevel(LogLevel::Warning);

  CFD_LOG_TRACE("test", "trace");
  CFD_LOG_DEBUG("test", "debug");
  CFD_LOG_INFO("test", "info");
  CFD_LOG_WARN("test", "warning");
  CFD_LOG_ERROR("test", "error");
  CFD_LOG_CRITICAL("test", "critical");

  const std::vector<LogRecord> records = sink_->snapshot();
  ASSERT_EQ(3u, records.size());
  EXPECT_EQ(LogLevel::Warning, records[0].level);
  EXPECT_EQ(LogLevel::Error, records[1].level);
  EXPECT_EQ(LogLevel::Critical, records[2].level);
}

TEST_F(LoggerTest, OffSuppressesEverything) {
  Logger::instance().setLevel(LogLevel::Off);

  CFD_LOG_CRITICAL("test", "the wing fell off");

  EXPECT_EQ(0u, sink_->size());
  EXPECT_FALSE(Logger::instance().shouldLog(LogLevel::Critical));
}

// This is the reason the macros exist rather than plain function calls.
// Once the solver logs a residual per iteration, a suppressed Trace message
// must not pay for formatting the numbers it would have printed.
TEST_F(LoggerTest, FilteredMessageDoesNotEvaluateItsArguments) {
  Logger::instance().setLevel(LogLevel::Error);

  int evaluations = 0;
  const auto expensive = [&evaluations]() {
    ++evaluations;
    return 1.0;
  };

  CFD_LOG_DEBUG("test", "value = {}", expensive());
  EXPECT_EQ(0, evaluations) << "arguments were evaluated for a filtered message";

  CFD_LOG_ERROR("test", "value = {}", expensive());
  EXPECT_EQ(1, evaluations);
}

TEST_F(LoggerTest, EverySinkSeesEveryRecord) {
  auto second = std::make_shared<RingBufferSink>(kCapacity);
  Logger::instance().addSink(second);

  CFD_LOG_INFO("test", "broadcast");

  EXPECT_EQ(2u, Logger::instance().sinkCount());
  EXPECT_EQ(1u, sink_->size());
  ASSERT_EQ(1u, second->size());
  EXPECT_EQ("broadcast", second->snapshot()[0].message);
}

TEST_F(LoggerTest, ShouldLogMatchesActualDelivery) {
  Logger::instance().setLevel(LogLevel::Info);

  EXPECT_FALSE(Logger::instance().shouldLog(LogLevel::Trace));
  EXPECT_FALSE(Logger::instance().shouldLog(LogLevel::Debug));
  EXPECT_TRUE(Logger::instance().shouldLog(LogLevel::Info));
  EXPECT_TRUE(Logger::instance().shouldLog(LogLevel::Warning));
  EXPECT_TRUE(Logger::instance().shouldLog(LogLevel::Critical));
}

TEST(RingBufferSinkTest, KeepsNewestRecordsAndCountsEvictions) {
  RingBufferSink sink{4};

  for (int i = 0; i < 7; ++i) {
    LogRecord record;
    record.level = LogLevel::Info;
    record.category = "test";
    record.message = std::to_string(i);
    sink.write(record);
  }

  const std::vector<LogRecord> records = sink.snapshot();
  ASSERT_EQ(4u, records.size());
  EXPECT_EQ("3", records.front().message);
  EXPECT_EQ("6", records.back().message);
  EXPECT_EQ(3u, sink.droppedCount());
}

TEST(RingBufferSinkTest, ClearResetsContentsAndDropCount) {
  RingBufferSink sink{2};
  for (int i = 0; i < 5; ++i) {
    sink.write(LogRecord{LogLevel::Info, "test", std::to_string(i), {}});
  }
  ASSERT_GT(sink.droppedCount(), 0u);

  sink.clear();

  EXPECT_EQ(0u, sink.size());
  EXPECT_EQ(0u, sink.droppedCount());
}

// The solver will run on a worker thread while the UI thread reads the buffer
// to draw the console. Without the mutex this loses records or corrupts the
// deque; with it, the count is exact.
TEST(RingBufferSinkTest, ConcurrentWritesAreNotLost) {
  constexpr int kThreads = 4;
  constexpr int kPerThread = 250;
  RingBufferSink sink{kThreads * kPerThread};

  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&sink, t]() {
      for (int i = 0; i < kPerThread; ++i) {
        sink.write(LogRecord{LogLevel::Info, "worker",
                             std::to_string(t) + ":" + std::to_string(i), {}});
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }

  EXPECT_EQ(static_cast<std::size_t>(kThreads * kPerThread), sink.size());
  EXPECT_EQ(0u, sink.droppedCount());
}

TEST(LogLevelParsing, AcceptsNamesCaseInsensitively) {
  EXPECT_EQ(LogLevel::Trace, cfd::logLevelFromString("trace"));
  EXPECT_EQ(LogLevel::Info, cfd::logLevelFromString("INFO"));
  EXPECT_EQ(LogLevel::Warning, cfd::logLevelFromString("Warn"));
  EXPECT_EQ(LogLevel::Warning, cfd::logLevelFromString("warning"));
  EXPECT_EQ(LogLevel::Critical, cfd::logLevelFromString("CRITICAL"));
  EXPECT_EQ(LogLevel::Off, cfd::logLevelFromString("off"));
}

TEST(LogLevelParsing, RejectsUnknownNames) {
  EXPECT_FALSE(cfd::logLevelFromString("verbose").has_value());
  EXPECT_FALSE(cfd::logLevelFromString("").has_value());
}

TEST(LogLevelParsing, RoundTripsThroughToString) {
  constexpr LogLevel kLevels[] = {LogLevel::Trace, LogLevel::Debug,
                                  LogLevel::Info,  LogLevel::Warning,
                                  LogLevel::Error, LogLevel::Critical,
                                  LogLevel::Off};
  for (const LogLevel level : kLevels) {
    EXPECT_EQ(level, cfd::logLevelFromString(cfd::toString(level)))
        << "failed for " << cfd::toString(level);
  }
}

TEST(Timestamp, IsFixedWidthClockTime) {
  const std::string text =
      cfd::formatTimestamp(std::chrono::system_clock::now());

  ASSERT_EQ(12u, text.size());  // HH:MM:SS.mmm
  EXPECT_EQ(':', text[2]);
  EXPECT_EQ(':', text[5]);
  EXPECT_EQ('.', text[8]);
}

}  // namespace
