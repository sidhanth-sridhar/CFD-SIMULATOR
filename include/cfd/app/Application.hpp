// Application.hpp - the window, the frame loop, and nothing else.
//
// The declaration deliberately mentions neither GLFW nor Dear ImGui. All of
// that lives behind a pointer to an incomplete Impl type (the "pimpl" idiom),
// which buys two things:
//
//   * Callers - today main.cpp, later a batch-mode driver - do not inherit
//     GUI headers transitively.
//   * Changing the windowing or GUI library does not force a rebuild of
//     everything that includes this header.

#pragma once

#include <memory>
#include <string>

#include "cfd/core/Error.hpp"
#include "cfd/core/Log.hpp"

namespace cfd::app {

/// Startup parameters. Kept as a plain struct so new options can be added
/// with designated initialisers without breaking existing call sites.
struct ApplicationOptions {
  int windowWidth{1600};
  int windowHeight{980};
  std::string windowTitle{};  ///< empty -> derived from BuildInfo

  /// Sink the log console panel reads from. The caller registers it with the
  /// Logger and passes it here, so the panel and the terminal are two views of
  /// one record stream rather than two independent logs. May be null, in which
  /// case the console renders empty.
  std::shared_ptr<RingBufferSink> logBuffer{};

  /// When set, render a few frames, write the window's own framebuffer to this
  /// path as a BMP, and exit. The application reads back its own pixels, so
  /// this works without screen-recording permission and without a compositor
  /// screenshot tool - useful for checking rendering over SSH or in CI, and as
  /// a baseline once there are flow fields worth comparing.
  std::string screenshotPath{};

  /// Frames to render before capturing. More than one because the first frame
  /// establishes layout and font atlases that later frames depend on.
  int screenshotAfterFrames{3};

  /// Log a frame-time summary every this many frames, or 0 to stay quiet.
  ///
  /// The status bar already shows the instantaneous cost, but a single number
  /// flickering past is no way to judge whether a change helped. A periodic
  /// mean and worst case over a fixed window is.
  int frameStatsEvery{0};

  /// Stop after this many frames. Zero runs until the window is closed. Exists
  /// so a timing run is a bounded, repeatable measurement rather than something
  /// that has to be interrupted by hand.
  long long maxFrames{0};

  /// Generate the computational mesh at startup, at the named resolution:
  /// "coarse", "medium" or "fine". Empty leaves meshing switched off.
  std::string initialMeshResolution{};

  /// Initialise the flow at startup. Requires a mesh, so it implies --mesh.
  bool initialiseFlow{false};

  /// Start the solver running at startup. Implies --flow.
  bool startSolver{false};

  /// Reynolds number based on the chord. Zero keeps the default.
  double reynoldsNumber{0.0};

  /// Outer-iteration ceiling for each solve. Zero keeps the default.
  long long maxIterations{0};

  /// Angle of attack in degrees. Zero is a legitimate value, so a separate
  /// flag says whether one was asked for at all.
  double angleOfAttackDeg{0.0};
  bool angleGiven{false};

  /// Run an angle-of-attack sweep at startup and exit when it finishes.
  /// Implies a mesh and a flow.
  bool runPolarSweep{false};
  double polarStartDeg{0.0};
  double polarEndDeg{18.0};
  double polarStepDeg{2.0};
  /// Where the sweep writes its CSV. Empty keeps the default.
  std::string polarCsvPath;

  /// Scalar shown in the viewport: "velocity", "vx", "vy", "pressure" or
  /// "divergence". Empty keeps the default.
  std::string initialFieldView{};

  /// Section to load at startup, e.g. "NACA 2412". Empty keeps the default.
  /// The string is placed in the geometry panel's input, so it is validated
  /// and reported through the same path as anything typed by hand.
  std::string initialSection{};
};

/// Owns the window, the GUI context and the main loop.
///
/// Lifetime is explicit rather than RAII-on-construction: initialize() can
/// fail (no display, no OpenGL 3.2) and a constructor cannot report that
/// through a Status. Construction is trivial; initialize() is where the real
/// work and the failure modes are.
class Application {
 public:
  Application();
  ~Application();

  Application(const Application&) = delete;
  Application& operator=(const Application&) = delete;
  Application(Application&&) noexcept;
  Application& operator=(Application&&) noexcept;

  /// Create the window and GUI context. Safe to call once.
  [[nodiscard]] Status initialize(const ApplicationOptions& options = {});

  /// Run until the user closes the window. Returns the process exit code.
  /// Requires a successful initialize().
  [[nodiscard]] int run();

  /// Release GPU and window resources. Idempotent; the destructor calls it.
  void shutdown() noexcept;

  [[nodiscard]] bool isInitialized() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cfd::app
