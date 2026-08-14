// Log.hpp - severity-filtered logging with pluggable sinks.
//
// Design notes
// ------------
// * Sinks, not hardcoded output. The same log record must be able to reach the
//   terminal *and* the console panel inside the GUI. A sink interface makes
//   that a matter of registration rather than special-casing.
//
// * Level check before formatting. The logging macros test the active level
//   before calling std::format. Once the solver is logging a residual every
//   iteration, a suppressed Trace message must cost a comparison and nothing
//   more - no string building, no allocation.
//
// * Thread-safe from the start. Phase 0 is single-threaded, but the solver
//   will eventually run on a worker thread while the UI redraws on the main
//   thread, and both will log. Retrofitting locking onto a logger that is
//   already used in fifty places is worse than paying for it now.

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cfd {

/// Ordered by increasing urgency; `Off` suppresses everything.
enum class LogLevel {
  Trace = 0,   ///< Per-iteration solver detail.
  Debug,       ///< Developer diagnostics.
  Info,        ///< Normal progress worth reporting to the user.
  Warning,     ///< Suspicious but survivable (poor mesh quality, clamped input).
  Error,       ///< An operation failed.
  Critical,    ///< The run cannot continue.
  Off,
};

[[nodiscard]] std::string_view toString(LogLevel level) noexcept;

/// Parse a level name case-insensitively ("info", "WARNING"). Returns nullopt
/// for unrecognised input so the caller can report a usage error.
[[nodiscard]] std::optional<LogLevel> logLevelFromString(std::string_view text) noexcept;

/// One emitted message. Copyable, because the GUI holds snapshots of these
/// while new records keep arriving.
struct LogRecord {
  LogLevel level{LogLevel::Info};
  std::string category;  ///< Subsystem tag, e.g. "app", "mesh", "solver".
  std::string message;
  std::chrono::system_clock::time_point timestamp{};
};

/// "14:22:31.184"
[[nodiscard]] std::string formatTimestamp(std::chrono::system_clock::time_point tp);

/// Destination for log records. Implementations must be safe to call from
/// multiple threads; the Logger does not serialise sink writes for you.
class LogSink {
 public:
  LogSink() = default;
  virtual ~LogSink() = default;

  LogSink(const LogSink&) = delete;
  LogSink& operator=(const LogSink&) = delete;
  LogSink(LogSink&&) = delete;
  LogSink& operator=(LogSink&&) = delete;

  virtual void write(const LogRecord& record) = 0;
};

/// Writes to the terminal. Warning and above go to stderr so that piping
/// stdout to a file still shows problems on the console.
class ConsoleSink final : public LogSink {
 public:
  /// `colour` defaults to auto-detection: ANSI codes only when stderr is a TTY.
  explicit ConsoleSink(std::optional<bool> colour = std::nullopt);

  void write(const LogRecord& record) override;

 private:
  std::mutex mutex_;
  bool colour_;
};

/// Keeps the most recent N records in memory. This is what backs the log
/// console in the GUI: the panel renders real records from this buffer, so
/// what the window shows and what the terminal shows cannot drift apart.
///
/// Bounded on purpose. A long solver run would otherwise grow this without
/// limit; instead the oldest records are discarded and counted.
class RingBufferSink final : public LogSink {
 public:
  explicit RingBufferSink(std::size_t capacity = 4096);

  void write(const LogRecord& record) override;

  [[nodiscard]] std::vector<LogRecord> snapshot() const;
  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  /// Number of records evicted because the buffer was full.
  [[nodiscard]] std::size_t droppedCount() const;
  void clear();

 private:
  mutable std::mutex mutex_;
  std::deque<LogRecord> records_;
  std::size_t capacity_;
  std::size_t dropped_{0};
};

/// Process-wide logger.
///
/// A singleton because logging is genuinely ambient: threading a logger
/// reference through every geometry and mesh routine would add a parameter to
/// most functions in the codebase for no analytical benefit. The sink list is
/// injectable, which preserves testability - tests attach their own sink and
/// assert on what was recorded.
class Logger {
 public:
  [[nodiscard]] static Logger& instance();

  void setLevel(LogLevel level) noexcept;
  [[nodiscard]] LogLevel level() const noexcept;

  /// True when a record at `level` would reach at least one sink.
  [[nodiscard]] bool shouldLog(LogLevel level) const noexcept;

  void addSink(std::shared_ptr<LogSink> sink);
  void clearSinks();
  [[nodiscard]] std::size_t sinkCount() const;

  /// Emit a fully formatted message. Prefer the CFD_LOG_* macros, which skip
  /// the formatting work entirely when the level is filtered out.
  void log(LogLevel level, std::string_view category, std::string message);

  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;
  Logger(Logger&&) = delete;
  Logger& operator=(Logger&&) = delete;

 private:
  Logger() = default;
  ~Logger() = default;

  mutable std::mutex mutex_;
  std::atomic<LogLevel> level_{LogLevel::Info};
  std::vector<std::shared_ptr<LogSink>> sinks_;
};

}  // namespace cfd

// ---------------------------------------------------------------------------
// Macros
// ---------------------------------------------------------------------------
// The do/while(false) wrapper makes the macro a single statement, so it
// behaves correctly as the body of an unbraced `if`.

#define CFD_LOG(level_, category_, ...)                                     \
  do {                                                                      \
    const ::cfd::LogLevel cfd_log_level_ = (level_);                        \
    if (::cfd::Logger::instance().shouldLog(cfd_log_level_)) {              \
      ::cfd::Logger::instance().log(cfd_log_level_, (category_),            \
                                    ::std::format(__VA_ARGS__));            \
    }                                                                       \
  } while (false)

#define CFD_LOG_TRACE(category_, ...) CFD_LOG(::cfd::LogLevel::Trace, category_, __VA_ARGS__)
#define CFD_LOG_DEBUG(category_, ...) CFD_LOG(::cfd::LogLevel::Debug, category_, __VA_ARGS__)
#define CFD_LOG_INFO(category_, ...) CFD_LOG(::cfd::LogLevel::Info, category_, __VA_ARGS__)
#define CFD_LOG_WARN(category_, ...) CFD_LOG(::cfd::LogLevel::Warning, category_, __VA_ARGS__)
#define CFD_LOG_ERROR(category_, ...) CFD_LOG(::cfd::LogLevel::Error, category_, __VA_ARGS__)
#define CFD_LOG_CRITICAL(category_, ...) CFD_LOG(::cfd::LogLevel::Critical, category_, __VA_ARGS__)
