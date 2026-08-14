#include "cfd/core/Log.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <utility>

#if defined(_WIN32)
#include <io.h>
#define CFD_ISATTY _isatty
#define CFD_FILENO _fileno
#else
#include <unistd.h>
#define CFD_ISATTY isatty
#define CFD_FILENO fileno
#endif

namespace cfd {
namespace {

/// ANSI SGR colours, muted rather than bright: this output sits alongside
/// build logs and shell prompts and should not shout.
constexpr std::string_view kReset = "\033[0m";

std::string_view colourFor(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::Trace:    return "\033[38;5;244m";  // grey
    case LogLevel::Debug:    return "\033[38;5;110m";  // dim blue
    case LogLevel::Info:     return "\033[38;5;252m";  // near-white
    case LogLevel::Warning:  return "\033[38;5;179m";  // amber
    case LogLevel::Error:    return "\033[38;5;167m";  // muted red
    case LogLevel::Critical: return "\033[1;38;5;167m";
    case LogLevel::Off:      return "";
  }
  return "";
}

bool isTerminal(std::FILE* stream) noexcept {
  return CFD_ISATTY(CFD_FILENO(stream)) != 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------

std::string_view toString(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::Trace:    return "TRACE";
    case LogLevel::Debug:    return "DEBUG";
    case LogLevel::Info:     return "INFO";
    case LogLevel::Warning:  return "WARN";
    case LogLevel::Error:    return "ERROR";
    case LogLevel::Critical: return "CRIT";
    case LogLevel::Off:      return "OFF";
  }
  return "?";
}

std::optional<LogLevel> logLevelFromString(std::string_view text) noexcept {
  std::string lowered;
  lowered.reserve(text.size());
  for (const char c : text) {
    lowered.push_back(static_cast<char>(
        std::tolower(static_cast<unsigned char>(c))));
  }

  // Both the short spelling used by the logger and the natural word are
  // accepted, so `--log-level warn` and `--log-level warning` both work.
  static const std::array<std::pair<std::string_view, LogLevel>, 9> kTable{{
      {"trace", LogLevel::Trace},
      {"debug", LogLevel::Debug},
      {"info", LogLevel::Info},
      {"warn", LogLevel::Warning},
      {"warning", LogLevel::Warning},
      {"error", LogLevel::Error},
      {"crit", LogLevel::Critical},
      {"critical", LogLevel::Critical},
      {"off", LogLevel::Off},
  }};

  for (const auto& [name, level] : kTable) {
    if (lowered == name) {
      return level;
    }
  }
  return std::nullopt;
}

std::string formatTimestamp(std::chrono::system_clock::time_point tp) {
  using namespace std::chrono;

  const auto since_epoch = tp.time_since_epoch();
  const auto secs = duration_cast<seconds>(since_epoch);
  const auto millis = duration_cast<milliseconds>(since_epoch - secs);

  const std::time_t as_time_t = system_clock::to_time_t(tp);
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &as_time_t);
#else
  localtime_r(&as_time_t, &local);
#endif

  return std::format("{:02}:{:02}:{:02}.{:03}", local.tm_hour, local.tm_min,
                     local.tm_sec, millis.count());
}

// ---------------------------------------------------------------------------
// ConsoleSink
// ---------------------------------------------------------------------------

ConsoleSink::ConsoleSink(std::optional<bool> colour)
    : colour_(colour.value_or(isTerminal(stderr))) {}

void ConsoleSink::write(const LogRecord& record) {
  // Warnings and worse go to stderr; routine progress goes to stdout. This
  // keeps `cfd_sim > run.log` from hiding problems.
  std::FILE* stream = (record.level >= LogLevel::Warning) ? stderr : stdout;

  const std::string line =
      std::format("[{}] {:<5} {:<8} {}\n", formatTimestamp(record.timestamp),
                  toString(record.level), record.category, record.message);

  // One lock per sink keeps interleaved lines from different threads intact.
  const std::lock_guard<std::mutex> guard(mutex_);
  if (colour_) {
    std::fputs(std::string{colourFor(record.level)}.c_str(), stream);
    std::fputs(line.c_str(), stream);
    std::fputs(std::string{kReset}.c_str(), stream);
  } else {
    std::fputs(line.c_str(), stream);
  }
  std::fflush(stream);
}

// ---------------------------------------------------------------------------
// RingBufferSink
// ---------------------------------------------------------------------------

RingBufferSink::RingBufferSink(std::size_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity) {}

void RingBufferSink::write(const LogRecord& record) {
  const std::lock_guard<std::mutex> guard(mutex_);
  if (records_.size() >= capacity_) {
    records_.pop_front();
    ++dropped_;
  }
  records_.push_back(record);
}

std::vector<LogRecord> RingBufferSink::snapshot() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return {records_.begin(), records_.end()};
}

std::size_t RingBufferSink::size() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return records_.size();
}

std::size_t RingBufferSink::droppedCount() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return dropped_;
}

void RingBufferSink::clear() {
  const std::lock_guard<std::mutex> guard(mutex_);
  records_.clear();
  dropped_ = 0;
}

// ---------------------------------------------------------------------------
// Logger
// ---------------------------------------------------------------------------

Logger& Logger::instance() {
  // Function-local static: initialised on first use, thread-safely, and
  // without the static-initialisation-order problem a namespace-scope global
  // would have.
  static Logger logger;
  return logger;
}

void Logger::setLevel(LogLevel level) noexcept {
  level_.store(level, std::memory_order_relaxed);
}

LogLevel Logger::level() const noexcept {
  return level_.load(std::memory_order_relaxed);
}

bool Logger::shouldLog(LogLevel level) const noexcept {
  const LogLevel active = level_.load(std::memory_order_relaxed);
  return active != LogLevel::Off && level != LogLevel::Off && level >= active;
}

void Logger::addSink(std::shared_ptr<LogSink> sink) {
  if (!sink) {
    return;
  }
  const std::lock_guard<std::mutex> guard(mutex_);
  sinks_.push_back(std::move(sink));
}

void Logger::clearSinks() {
  const std::lock_guard<std::mutex> guard(mutex_);
  sinks_.clear();
}

std::size_t Logger::sinkCount() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return sinks_.size();
}

void Logger::log(LogLevel level, std::string_view category, std::string message) {
  if (!shouldLog(level)) {
    return;
  }

  LogRecord record;
  record.level = level;
  record.category = std::string{category};
  record.message = std::move(message);
  record.timestamp = std::chrono::system_clock::now();

  // Copy the sink list under the lock, then write outside it. A sink that
  // blocks (or logs re-entrantly) must not hold up every other thread, and
  // must not deadlock on our own mutex.
  std::vector<std::shared_ptr<LogSink>> sinks;
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    sinks = sinks_;
  }

  for (const auto& sink : sinks) {
    sink->write(record);
  }
}

}  // namespace cfd
