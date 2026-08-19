// Entry point: parse the command line, start logging, hand off to the app.
//
// Kept thin deliberately. Everything here is process plumbing - argument
// parsing, sink setup, exit codes - and none of it belongs in a library that
// a future batch-mode driver or test harness would also want to use.

#include <cstdio>
#include <cstdlib>
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
  int screenshotFrames{0};
  int frameStatsEvery{0};
  long long maxFrames{0};
  std::string section;
  std::string meshResolution;
  bool initialiseFlow{false};
  bool startSolver{false};
  double reynoldsNumber{0.0};
  long long solverMaxIterations{0};
  std::string turbulenceModel;
  std::string convectionScheme;
  double turbulenceIntensity{0.0};
  double turbulenceViscosityRatio{0.0};
  double turbulenceLengthScale{0.0};
  double dynamicViscosity{0.0};
  double firstLayerHeight{0.0};
  double farFieldChords{0.0};
  double wakeChords{0.0};
  double angleOfAttackDeg{0.0};
  bool angleGiven{false};
  bool runPolarSweep{false};
  double polarStart{0.0};
  double polarEnd{18.0};
  double polarStep{2.0};
  std::string polarCsvPath;
  std::string fieldView;
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
      "      --section NAME      Load a NACA four-digit section at startup,\n"
      "                          e.g. --section \"NACA 2412\"\n"
      "      --mesh LEVEL        Generate the mesh at startup:\n"
      "                          coarse, medium or fine\n"
      "      --flow              Initialise the flow at startup (implies --mesh)\n"
      "      --solve             Start the solver running (implies --flow)\n"
      "      --reynolds N        Reynolds number based on the chord\n"
      "      --alpha DEG         Angle of attack in degrees\n"
      "      --max-iterations N  Outer-iteration ceiling for each solve\n"
      "      --turbulence NAME   Closure: laminar (default) or sst\n"
      "      --scheme NAME       Convection: upwind (default) or second\n"
      "      --intensity I       Freestream turbulence intensity, u'/U\n"
      "      --eddy-ratio R      Freestream mu_t/mu\n"
      "      --length-scale L    Turbulent length scale in metres (instead of R)\n"
      "      --viscosity MU      Dynamic viscosity in Pa.s; Reynolds then follows\n"
      "      --first-layer H     First cell height off the wall, in chords\n"
      "      --far-field N       Upstream and vertical extent, in chords\n"
      "      --wake N            Downstream extent, in chords\n"
      "      --polar A:B:S       Sweep incidence from A to B in steps of S degrees,\n"
      "                          write the polar and exit (implies --flow)\n"
      "      --polar-csv FILE    Where the sweep writes its CSV\n"
      "      --field NAME        Shown scalar: velocity, vx, vy, pressure, cp,\n"
      "                          vorticity, k, omega, mut or divergence\n"
      "      --frame-stats N     Log a frame-time summary every N frames\n"
      "      --max-frames N      Stop after N frames (for timing runs)\n"
      "      --self-check        Run headless startup checks and exit\n"
      "      --screenshot FILE   Render a few frames, save the window to FILE\n"
      "                          as a BMP, then exit\n"
      "      --screenshot-frames N\n"
      "                          Frames to render first (default 3). Use a large\n"
      "                          value with --solve to capture a converged run\n"
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
    } else if (arg == "--section") {
      if (i + 1 >= args.size()) {
        return cfd::Error{cfd::ErrorCode::InvalidArgument,
                          "--section requires a designation, e.g. \"NACA 2412\""};
      }
      ++i;
      options.section = std::string{args[i]};
    } else if (arg == "--field") {
      if (i + 1 >= args.size()) {
        return cfd::Error{cfd::ErrorCode::InvalidArgument,
                          "--field requires a name: velocity, vx, vy, pressure or divergence"};
      }
      ++i;
      options.fieldView = std::string{args[i]};
    } else if (arg == "--reynolds") {
      if (i + 1 >= args.size()) {
        return cfd::Error{cfd::ErrorCode::InvalidArgument, "--reynolds requires a value"};
      }
      ++i;
      options.reynoldsNumber = std::strtod(std::string{args[i]}.c_str(), nullptr);
      if (!(options.reynoldsNumber > 0.0)) {
        return cfd::Error{cfd::ErrorCode::InvalidArgument,
                          "--reynolds must be a positive number"};
      }
    } else if (arg == "--turbulence") {
      if (i + 1 >= args.size()) {
        return cfd::Error{cfd::ErrorCode::InvalidArgument,
                          "--turbulence requires a name: laminar or sst"};
      }
      ++i;
      options.turbulenceModel = std::string{args[i]};
    } else if (arg == "--intensity" || arg == "--eddy-ratio" ||
               arg == "--length-scale" || arg == "--viscosity") {
      const std::string_view which = arg;
      if (i + 1 >= args.size()) {
        return cfd::Error{cfd::ErrorCode::InvalidArgument,
                          std::string{which} + " requires a value"};
      }
      ++i;
      const double value = std::strtod(std::string{args[i]}.c_str(), nullptr);
      if (!(value > 0.0)) {
        return cfd::Error{cfd::ErrorCode::InvalidArgument,
                          std::string{which} + " must be a positive number"};
      }
      if (which == "--intensity") {
        options.turbulenceIntensity = value;
      } else if (which == "--eddy-ratio") {
        options.turbulenceViscosityRatio = value;
      } else if (which == "--length-scale") {
        options.turbulenceLengthScale = value;
      } else {
        options.dynamicViscosity = value;
      }
    } else if (arg == "--scheme") {
      if (i + 1 >= args.size()) {
        return cfd::Error{cfd::ErrorCode::InvalidArgument,
                          "--scheme requires a name: upwind or second"};
      }
      ++i;
      options.convectionScheme = std::string{args[i]};
    } else if (arg == "--first-layer") {
      if (i + 1 >= args.size()) {
        return cfd::Error{cfd::ErrorCode::InvalidArgument,
                          "--first-layer requires a height in chords"};
      }
      ++i;
      options.firstLayerHeight = std::strtod(std::string{args[i]}.c_str(), nullptr);
      if (!(options.firstLayerHeight > 0.0)) {
        return cfd::Error{cfd::ErrorCode::InvalidArgument,
                          "--first-layer must be a positive fraction of chord"};
      }
    } else if (arg == "--far-field" || arg == "--wake") {
      const bool isWake = arg == "--wake";
      if (i + 1 >= args.size()) {
        return cfd::Error{cfd::ErrorCode::InvalidArgument,
                          "--far-field and --wake require an extent in chords"};
      }
      ++i;
      const double chords = std::strtod(std::string{args[i]}.c_str(), nullptr);
      if (!(chords > 0.0)) {
        return cfd::Error{cfd::ErrorCode::InvalidArgument,
                          "the domain extent must be a positive number of chords"};
      }
      (isWake ? options.wakeChords : options.farFieldChords) = chords;
    } else if (arg == "--max-iterations") {
      if (i + 1 >= args.size()) {
        return cfd::Error{cfd::ErrorCode::InvalidArgument,
                          "--max-iterations requires a count"};
      }
      ++i;
      options.solverMaxIterations = std::atoll(std::string{args[i]}.c_str());
      if (options.solverMaxIterations < 1) {
        return cfd::Error{cfd::ErrorCode::InvalidArgument,
                          "--max-iterations must be at least 1"};
      }
    } else if (arg == "--alpha") {
      if (i + 1 >= args.size()) {
        return cfd::Error{cfd::ErrorCode::InvalidArgument, "--alpha requires a value in degrees"};
      }
      ++i;
      const std::string text{args[i]};
      char* end = nullptr;
      options.angleOfAttackDeg = std::strtod(text.c_str(), &end);
      if (end == text.c_str() || *end != '\0') {
        return cfd::Error{cfd::ErrorCode::InvalidArgument,
                          "--alpha must be a number of degrees"};
      }
      options.angleGiven = true;
    } else if (arg == "--polar") {
      if (i + 1 >= args.size()) {
        return cfd::Error{cfd::ErrorCode::InvalidArgument,
                          "--polar requires a range, e.g. --polar 0:18:2"};
      }
      ++i;
      const std::string spec{args[i]};
      const std::size_t first = spec.find(':');
      const std::size_t second = (first == std::string::npos)
                                     ? std::string::npos
                                     : spec.find(':', first + 1);
      if (first == std::string::npos || second == std::string::npos) {
        return cfd::Error{cfd::ErrorCode::InvalidArgument,
                          "--polar takes start:end:step, e.g. 0:18:2"};
      }
      options.polarStart = std::strtod(spec.substr(0, first).c_str(), nullptr);
      options.polarEnd = std::strtod(spec.substr(first + 1, second - first - 1).c_str(),
                                     nullptr);
      options.polarStep = std::strtod(spec.substr(second + 1).c_str(), nullptr);
      if (!(options.polarStep > 0.0)) {
        return cfd::Error{cfd::ErrorCode::InvalidArgument,
                          "--polar needs a positive step"};
      }
      options.runPolarSweep = true;
    } else if (arg == "--polar-csv") {
      if (i + 1 >= args.size()) {
        return cfd::Error{cfd::ErrorCode::InvalidArgument, "--polar-csv requires a path"};
      }
      ++i;
      options.polarCsvPath = std::string{args[i]};
    } else if (arg == "--solve") {
      options.startSolver = true;
    } else if (arg == "--flow") {
      options.initialiseFlow = true;
    } else if (arg == "--mesh") {
      if (i + 1 >= args.size()) {
        return cfd::Error{cfd::ErrorCode::InvalidArgument,
                          "--mesh requires a level: coarse, medium or fine"};
      }
      ++i;
      options.meshResolution = std::string{args[i]};
    } else if (arg == "--screenshot") {
      if (i + 1 >= args.size()) {
        return cfd::Error{cfd::ErrorCode::InvalidArgument,
                          "--screenshot requires a file path"};
      }
      ++i;
      options.screenshotPath = std::string{args[i]};
    } else if (arg == "--screenshot-frames") {
      if (i + 1 >= args.size()) {
        return cfd::Error{cfd::ErrorCode::InvalidArgument,
                          "--screenshot-frames requires a count"};
      }
      ++i;
      options.screenshotFrames = std::atoi(std::string{args[i]}.c_str());
      if (options.screenshotFrames < 1) {
        return cfd::Error{cfd::ErrorCode::InvalidArgument,
                          "--screenshot-frames must be at least 1"};
      }
    } else if (arg == "--frame-stats") {
      if (i + 1 >= args.size()) {
        return cfd::Error{cfd::ErrorCode::InvalidArgument, "--frame-stats requires a count"};
      }
      ++i;
      options.frameStatsEvery = std::atoi(std::string{args[i]}.c_str());
      if (options.frameStatsEvery < 1) {
        return cfd::Error{cfd::ErrorCode::InvalidArgument,
                          "--frame-stats must be at least 1"};
      }
    } else if (arg == "--max-frames") {
      if (i + 1 >= args.size()) {
        return cfd::Error{cfd::ErrorCode::InvalidArgument, "--max-frames requires a count"};
      }
      ++i;
      options.maxFrames = std::atoll(std::string{args[i]}.c_str());
      if (options.maxFrames < 1) {
        return cfd::Error{cfd::ErrorCode::InvalidArgument, "--max-frames must be at least 1"};
      }
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
    if (options.screenshotFrames > 0) {
      appOptions.screenshotAfterFrames = options.screenshotFrames;
    }
    appOptions.initialSection = options.section;
    appOptions.initialMeshResolution = options.meshResolution;
    appOptions.initialiseFlow = options.initialiseFlow;
    appOptions.initialFieldView = options.fieldView;
    appOptions.startSolver = options.startSolver;
    appOptions.reynoldsNumber = options.reynoldsNumber;
    appOptions.maxIterations = options.solverMaxIterations;
    appOptions.turbulenceModel = options.turbulenceModel;
    appOptions.convectionScheme = options.convectionScheme;
    appOptions.turbulenceIntensity = options.turbulenceIntensity;
    appOptions.turbulenceViscosityRatio = options.turbulenceViscosityRatio;
    appOptions.turbulenceLengthScale = options.turbulenceLengthScale;
    appOptions.dynamicViscosity = options.dynamicViscosity;
    appOptions.firstLayerHeight = options.firstLayerHeight;
    appOptions.farFieldChords = options.farFieldChords;
    appOptions.wakeChords = options.wakeChords;
    appOptions.angleOfAttackDeg = options.angleOfAttackDeg;
    appOptions.angleGiven = options.angleGiven;
    appOptions.runPolarSweep = options.runPolarSweep;
    appOptions.polarStartDeg = options.polarStart;
    appOptions.polarEndDeg = options.polarEnd;
    appOptions.polarStepDeg = options.polarStep;
    appOptions.polarCsvPath = options.polarCsvPath;
    appOptions.frameStatsEvery = options.frameStatsEvery;
    appOptions.maxFrames = options.maxFrames;

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
