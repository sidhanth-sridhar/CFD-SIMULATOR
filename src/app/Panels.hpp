// Panels.hpp - the contents of each region of the window.
//
// Split from Application.cpp on a clear seam: Application decides *where* the
// regions are (layout, splitters, the frame loop), these functions decide
// *what* is drawn inside them. Each takes the shared UiState and assumes an
// ImGui window is already current.
//
// Everything displayed here is a real, measured value: build metadata baked in
// at compile time, strings queried from the OpenGL driver, live camera state,
// and actual log records. Nothing is mocked up. Where a capability does not
// exist yet, the UI says so rather than showing a plausible-looking number.

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "Theme.hpp"
#include "cfd/app/Camera2D.hpp"
#include "cfd/core/Log.hpp"

namespace cfd::app {

/// Strings reported by the OpenGL driver, captured once after context
/// creation. Useful when a rendering bug turns out to be driver-specific.
struct RendererInfo {
  std::string vendor{"unknown"};
  std::string renderer{"unknown"};
  std::string glVersion{"unknown"};
  std::string glslVersion{"unknown"};
  std::string windowSystem{"unknown"};  ///< GLFW runtime version string
};

/// Everything the UI reads or mutates during a frame.
///
/// A single plain struct rather than a web of widget objects: with immediate
/// mode there is no retained widget tree to keep in sync, so the honest
/// representation of "the UI" is just the data it draws from.
struct UiState {
  // --- layout (unscaled pixels) ---
  float leftPanelWidth{330.0f};
  float consoleHeight{210.0f};
  bool showLeftPanel{true};
  bool showConsole{true};

  // --- viewport ---
  Camera2D camera;
  /// The default framing depends on the viewport's pixel size, which is not
  /// known until the first frame has been laid out. Fit once, then leave the
  /// camera alone so user panning and zooming persist.
  bool viewInitialized{false};
  bool showGrid{true};
  bool showAxes{true};
  bool showScaleBar{true};
  bool cursorInViewport{false};
  Vec2 cursorWorld{};

  // --- console ---
  bool autoScroll{true};
  std::vector<LogRecord> consoleCache;
  std::size_t cachedRecordCount{0};
  std::size_t cachedDroppedCount{0};

  // --- misc ---
  bool aboutRequested{false};
  bool quitRequested{false};
  /// CPU milliseconds spent building and submitting the previous frame. This
  /// is deliberately not the interval between frames: the loop idles on
  /// glfwWaitEventsTimeout, so wall-clock frame spacing mostly measures how
  /// long the user sat still.
  float lastFrameCpuMs{0.0f};

  theme::Fonts fonts;
  RendererInfo renderer;
  std::shared_ptr<RingBufferSink> logBuffer;
};

/// Restore the default view: origin centred, unit chord comfortably framed.
void resetView(UiState& ui);

/// Restore default panel sizes and visibility.
void resetLayout(UiState& ui);

// Each of these assumes a current ImGui window (or, for the menu bar, an
// active main menu bar).
void drawMenuBar(UiState& ui);
void drawToolbar(UiState& ui);
void drawSessionPanel(UiState& ui);
void drawViewport(UiState& ui);
void drawConsole(UiState& ui);
void drawStatusBar(UiState& ui);

/// Opens on demand; call once per frame from the top level.
void drawAboutModal(UiState& ui);

}  // namespace cfd::app
