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

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Theme.hpp"
#include "cfd/app/Camera2D.hpp"
#include "cfd/core/Log.hpp"
#include "cfd/geom/Airfoil.hpp"
#include "cfd/mesh/CGrid.hpp"
#include "cfd/mesh/Mesh.hpp"

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

/// Airfoil inputs, the section they produced, and how it is drawn.
struct GeometryState {
  /// Typed by the user. A fixed buffer because ImGui::InputText works on one;
  /// 32 characters is far more than "NACA 2412" needs. Seeded with a cambered
  /// section so the window shows a recognisable airfoil on first launch.
  std::array<char, 32> designation{"2412"};

  int pointsPerSurface{121};
  double chord{1.0};
  /// Closed by default, because that is the form the mesher can wrap: a C-grid
  /// needs the wake cut to start from a single point. The published open form
  /// stays available for comparison against reference ordinates.
  geom::TrailingEdge trailingEdge{geom::TrailingEdge::Closed};

  // Display toggles.
  bool fillSection{true};
  bool showCamberLine{true};
  bool showChordLine{true};
  bool showSurfacePoints{false};

  /// The last section that generated successfully. Kept on screen while the
  /// user is midway through typing a new designation, so the view does not
  /// flash empty on every keystroke.
  std::optional<geom::Airfoil> airfoil;

  /// Why the current input did not generate, empty when it did.
  std::string errorMessage;

  /// Set whenever an input changes; consumed by updateGeometry().
  bool dirty{true};
};

/// Mesh inputs, the grid they produced, and how it is drawn.
struct MeshState {
  bool enabled{false};
  bool showInterior{true};
  bool showBoundaries{true};

  mesh::MeshResolution resolution{mesh::MeshResolution::Medium};

  // Domain extent in chord lengths, independent of the resolution preset.
  double upstreamChords{12.0};
  double downstreamChords{25.0};
  double verticalChords{12.0};
  /// Seeded from the resolution preset, then editable.
  double firstLayerHeight{3.0e-4};

  std::optional<mesh::Mesh> mesh;
  std::string errorMessage;
  bool dirty{true};

  /// Wall-clock cost of the last successful generation.
  double lastGenerationMs{0.0};
  /// Frame the whole domain once the next mesh is ready. Used at startup when
  /// a mesh was requested on the command line, where showing the section alone
  /// would hide what was asked for.
  bool pendingDomainFit{false};
  /// Set by the renderer: 1 when every grid line is drawn, higher when the
  /// view is decimated for speed. Reported in the panel so a thinned-out mesh
  /// is never mistaken for the real one.
  int drawStride{1};

  [[nodiscard]] mesh::CGridOptions options() const {
    mesh::CGridOptions o = mesh::optionsFor(resolution);
    o.upstreamChords = upstreamChords;
    o.downstreamChords = downstreamChords;
    o.verticalChords = verticalChords;
    o.firstLayerHeight = firstLayerHeight;
    return o;
  }
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
  /// Trackpad magnification collected since the previous frame, as a relative
  /// change. Drained once per frame by the application so that the viewport is
  /// the only thing that consumes it.
  double pinchMagnification{0.0};

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

  GeometryState geometry;
  MeshState meshing;

  theme::Fonts fonts;
  RendererInfo renderer;
  std::shared_ptr<RingBufferSink> logBuffer;
};

/// Regenerate the section if any geometry input changed. Call once per frame
/// before the panels are drawn, so the viewport and the readout never show
/// results from different inputs.
void updateGeometry(UiState& ui);

/// Regenerate the mesh if the geometry or any meshing input changed. Must run
/// after updateGeometry, since the grid is built around the current section.
void updateMesh(UiState& ui);

/// Restore the default view: origin centred, unit chord comfortably framed.
void resetView(UiState& ui);

/// Frame the whole computational domain. Does nothing without a mesh.
void fitDomain(UiState& ui);

/// Restore default panel sizes and visibility.
void resetLayout(UiState& ui);

// Each of these assumes a current ImGui window (or, for the menu bar, an
// active main menu bar).
void drawMenuBar(UiState& ui);
void drawToolbar(UiState& ui);
void drawGeometryPanel(UiState& ui);
void drawMeshPanel(UiState& ui);
void drawSessionPanel(UiState& ui);
void drawViewport(UiState& ui);
void drawConsole(UiState& ui);
void drawStatusBar(UiState& ui);

/// Opens on demand; call once per frame from the top level.
void drawAboutModal(UiState& ui);

}  // namespace cfd::app
