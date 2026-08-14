// Entry point: parse the command line, start logging, hand off to the app.
//
// Kept thin deliberately. Everything here is process plumbing - argument
// parsing, sink setup, exit codes - and none of it belongs in a library that
// a future batch-mode driver or test harness would also want to use.

#include <cstdio>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cfd/app/Application.hpp"
#include "cfd/core/BuildInfo.hpp"
#include "cfd/core/Error.hpp"
#include "cfd/core/Log.hpp"

namespace {

constexpr std::string_view kLogCategory = "main";

// Conventional shell exit codes: 0 success, 1 runtime failure, 2 usage error.
constexpr int kExitSuccess = 0;
constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;

struct Options {
  cfd::LogLevel logLevel{cfd::LogLevel::Info};
  bool showHelp{false};
  bool showVersion{false};
  bool selfCheck{false};
  std::string screenshotPath;
};

void printUsage() {
  std::printf(
      "%s %s - %s\n"
      "\n"
      "Usage: cfd_sim [options]\n"
      "\n"
      "Options:\n"
      "  -h, --help              Show this message and exit\n"
      "  -V, --version           Show build information and exit\n"
      "      --log-level LEVEL   trace|debug|info|warn|error|critical|off\n"
      "                          (default: info)\n"
      "      --self-check        Run headless startup checks and exit\n"
      "      --screenshot FILE   Render a few frames, save the window to FILE\n"
      "                          as a BMP, then exit\n"
      "\n"
      "With no options the graphical application starts.\n",
      cfd::BuildInfo::projectName().data(), cfd::BuildInfo::version().data(),
      cfd::BuildInfo::description().data());
}

cfd::Result<Options> parseArguments(std::span<const std::string_view> args) {
  Options options;

  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view arg = args[i];

    if (arg == "-h" || arg == "--help") {
      options.showHelp = true;
    } else if (arg == "-V" || arg == "--version") {
      options.showVersion = true;
    } else if (arg == "--self-check") {
      options.selfCheck = true;
    } else if (arg == "--screenshot") {
      if (i + 1 >= args.size()) {
        return cfd::Error{cfd::ErrorCode::InvalidArgument,
                          "--screenshot requires a file path"};
      }
      ++i;
      options.screenshotPath = std::string{args[i]};
    } else if (arg == "--log-level") {
      if (i + 1 >= args.size()) {
        return cfd::Error{cfd::ErrorCode::InvalidArgument,
                          "--log-level requires a value"};
      }
      ++i;
      const auto level = cfd::logLevelFromString(args[i]);
      if (!level.has_value()) {
        return cfd::Error{cfd::ErrorCode::InvalidArgument,
                          "unknown log level '" + std::string{args[i]} + "'"};
      }
      options.logLevel = *level;
    } else {
      return cfd::Error{cfd::ErrorCode::InvalidArgument,
                        "unrecognised option '" + std::string{arg} + "'"};
    }
  }

  return options;
}

/// Headless verification that the binary and its core subsystems are sound.
///
/// This is what CTest runs, so it must not need a display, a GPU or a window
/// server. It therefore checks the parts that work anywhere - logging
/// delivery, error propagation, build metadata - and deliberately does not
/// try to open a window. Graphics initialisation is genuinely untested here,
/// and pretending otherwise would make the test worthless.
cfd::Status runSelfCheck() {
  auto buffer = std::make_shared<cfd::RingBufferSink>(64);
  cfd::Logger::instance().addSink(buffer);

  CFD_LOG_INFO(kLogCategory, "self-check: logging");
  if (buffer->size() != 1) {
    return cfd::Error{cfd::ErrorCode::Internal,
                      "log record did not reach the registered sink"};
  }

  // A filtered message must not be delivered.
  const cfd::LogLevel restore = cfd::Logger::instance().level();
  cfd::Logger::instance().setLevel(cfd::LogLevel::Error);
  CFD_LOG_DEBUG(kLogCategory, "this must be suppressed");
  cfd::Logger::instance().setLevel(restore);
  if (buffer->size() != 1) {
    return cfd::Error{cfd::ErrorCode::Internal, "severity filtering is not applied"};
  }

  if (cfd::BuildInfo::version().empty() || cfd::BuildInfo::buildType() == "Unknown") {
    return cfd::Error{cfd::ErrorCode::Internal, "build metadata was not populated"};
  }

  const cfd::Status failure =
      cfd::Error{cfd::ErrorCode::NotImplemented, "self-check probe"};
  if (failure.hasValue() || failure.error().code() != cfd::ErrorCode::NotImplemented) {
    return cfd::Error{cfd::ErrorCode::Internal, "Status does not carry errors correctly"};
  }

  CFD_LOG_INFO(kLogCategory, "self-check: {} {} ({}) OK", cfd::BuildInfo::projectName(),
               cfd::BuildInfo::version(), cfd::BuildInfo::buildType());
  return cfd::Status::ok();
}

}  // namespace

int main(int argc, char** argv) {
  // A top-level catch is the last line of defence: an uncaught exception
  // would otherwise terminate with no diagnostic at all.
  try {
    std::vector<std::string_view> args;
    args.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int i = 1; i < argc; ++i) {
      args.emplace_back(argv[i]);
    }

    const cfd::Result<Options> parsed = parseArguments(args);
    if (!parsed) {
      std::fprintf(stderr, "error: %s\n\n", parsed.error().message().c_str());
      printUsage();
      return kExitUsage;
    }
    const Options& options = parsed.value();

    if (options.showHelp) {
      printUsage();
      return kExitSuccess;
    }
    if (options.showVersion) {
      std::fputs(cfd::BuildInfo::summary().c_str(), stdout);
      return kExitSuccess;
    }

    cfd::Logger::instance().setLevel(options.logLevel);
    cfd::Logger::instance().addSink(std::make_shared<cfd::ConsoleSink>());

    if (options.selfCheck) {
      if (const cfd::Status status = runSelfCheck(); !status) {
        CFD_LOG_CRITICAL(kLogCategory, "self-check failed: {}",
                         status.error().format());
        return kExitFailure;
      }
      return kExitSuccess;
    }

    // The GUI keeps its own copy of the log so the console panel shows exactly
    // what the terminal shows.
    auto guiLog = std::make_shared<cfd::RingBufferSink>(4096);
    cfd::Logger::instance().addSink(guiLog);

    cfd::app::ApplicationOptions appOptions;
    appOptions.logBuffer = guiLog;
    appOptions.screenshotPath = options.screenshotPath;

    cfd::app::Application application;
    if (const cfd::Status status = application.initialize(appOptions); !status) {
      CFD_LOG_CRITICAL(kLogCategory, "startup failed: {}", status.error().format());
      return kExitFailure;
    }

    return application.run();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "fatal: unhandled exception: %s\n", error.what());
    return kExitFailure;
  } catch (...) {
    std::fprintf(stderr, "fatal: unhandled non-standard exception\n");
    return kExitFailure;
  }
}
