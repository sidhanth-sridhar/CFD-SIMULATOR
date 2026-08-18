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
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "FieldRenderer.hpp"
#include "SolverWorker.hpp"
#include "Theme.hpp"
#include "cfd/app/Camera2D.hpp"
#include "cfd/core/Log.hpp"
#include "cfd/geom/Airfoil.hpp"
#include "cfd/flow/BoundaryConditions.hpp"
#include "cfd/flow/FlowField.hpp"
#include "cfd/flow/Freestream.hpp"
#include "cfd/mesh/CGrid.hpp"
#include "cfd/mesh/Mesh.hpp"
#include "cfd/post/Forces.hpp"
#include "cfd/post/Polar.hpp"
#include "cfd/post/Streamlines.hpp"
#include "cfd/post/SurfaceData.hpp"
#include "cfd/solver/SimpleSolver.hpp"

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

/// Everything about where the camera is and how big the canvas is.
///
/// Screen-space geometry is only valid for one of these. Caching it and
/// comparing the key is what lets the viewport skip rebuilding drawings that
/// would come out identical - which, while a solve runs and nobody is touching
/// the mouse, is all of them.
struct ViewKey {
  std::uint64_t meshRevision{0};
  Vec2 cameraCentre{};
  double pixelsPerUnit{0.0};
  float originX{0.0f};
  float originY{0.0f};
  float width{0.0f};
  float height{0.0f};

  [[nodiscard]] bool operator==(const ViewKey&) const = default;
};

/// Grid lines, already projected to the screen.
///
/// Held as one flat point array plus the length of each run rather than a
/// vector of vectors: at the fine resolution this is tens of thousands of
/// segments, and the allocation churn of the obvious representation costs more
/// than the projection it is meant to save.
struct GridLineCache {
  ViewKey key;
  bool valid{false};
  std::vector<ImVec2> points;
  std::vector<int> runs;
  int stride{1};
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

  /// Shared, and const, because the solver runs on its own thread and holds a
  /// reference to the grid it was built on. Regenerating the mesh here swaps
  /// this pointer; the worker's copy keeps the old grid alive for exactly as
  /// long as it is still reading from it. A bare optional would leave the
  /// worker pointing at freed memory the moment a resolution changed.
  std::shared_ptr<const mesh::Mesh> mesh;
  std::string errorMessage;
  bool dirty{true};

  /// Bumped on every successful regeneration. Cheap for view caches to compare
  /// against, and unlike the pointer it never repeats after a reallocation.
  std::uint64_t revision{0};

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

  /// Projected grid lines, reused until the mesh or the view changes.
  GridLineCache lines;

  [[nodiscard]] mesh::CGridOptions options() const {
    mesh::CGridOptions o = mesh::optionsFor(resolution);
    o.upstreamChords = upstreamChords;
    o.downstreamChords = downstreamChords;
    o.verticalChords = verticalChords;
    o.firstLayerHeight = firstLayerHeight;
    return o;
  }
};

/// Which scalar the viewport shades cells by.
enum class FieldView {
  VelocityMagnitude,
  VelocityX,
  VelocityY,
  Pressure,
  /// Net volume flux per unit area. Zero everywhere for an incompressible
  /// solution, so any structure here is error - which makes it the most
  /// informative thing to look at before a solver exists.
  Divergence,
};

[[nodiscard]] std::string_view toString(FieldView view) noexcept;
/// True where the sign of the quantity carries meaning, so it wants a
/// diverging colour map centred on zero.
[[nodiscard]] bool isSignedField(FieldView view) noexcept;

/// Flow inputs, the initialised state, and how it is displayed.
struct FlowState {
  bool enabled{false};

  flow::FreestreamConditions freestream;
  flow::BoundaryConditions conditions;

  std::optional<flow::FlowField> field;
  std::optional<flow::FaceState> faces;

  /// Bumped every time the field's values change. The viewport caches an image
  /// of the shaded field and needs a cheap way to ask "is what I drew still
  /// what is here?"; comparing 105,410 cells would defeat the purpose.
  std::uint64_t revision{0};
  std::vector<double> divergence;
  flow::ResidualSet residuals;
  flow::TimeState clock;
  flow::ResidualHistory history;

  /// How many faces ended up carrying each condition. Counted once when the
  /// conditions are applied rather than rescanned every frame.
  struct BoundaryCounts {
    std::size_t wall{0};
    std::size_t inlet{0};
    std::size_t outlet{0};
    std::size_t farFieldIn{0};
    std::size_t farFieldOut{0};
    std::size_t internalCut{0};
  };
  BoundaryCounts counts;

  std::string errorMessage;
  bool dirty{true};

  /// Carry the current velocity and pressure into the rebuilt field instead of
  /// starting again from the undisturbed stream.
  ///
  /// Set when only the freestream changed. The steady solution does not depend
  /// on what it was started from, so continuing from the answer at 8 degrees is
  /// as valid a route to the answer at 9 as starting cold - and enormously
  /// cheaper, which is what makes sweeping incidence with a slider usable at
  /// all rather than a way to discard a thousand iterations per nudge.
  ///
  /// Cleared by the flow rebuild, so it has to be set again for each change.
  bool warmStart{false};

  /// Whether the field currently on screen was carried over rather than
  /// started cold. Reported next to the iteration count, so that a case that
  /// converged in 200 iterations from a neighbouring solution is not mistaken
  /// for one that converged in 200 iterations from scratch.
  bool continuedFromPrevious{false};

  // --- display ---
  bool showField{true};
  bool showBoundaryKinds{true};
  FieldView view{FieldView::VelocityMagnitude};
  /// Auto-scale the colour map to the data. Turned off to compare two states
  /// on the same scale.
  bool autoRange{true};
  double rangeMin{0.0};
  double rangeMax{1.0};

  /// Cells actually shaded, and whether the last frame redrew them or reused
  /// the cached image.
  std::size_t shadedCells{0};
  bool shadingComplete{true};
  bool shadingRebuilt{false};

  /// Owns the offscreen texture the shaded field is cached in. Lives here
  /// because it is per-view state, but it holds OpenGL objects, so the
  /// application releases it explicitly while the context is still current.
  FieldRenderer renderer;
};

/// The solver, its controls and its convergence history.
struct SolverState {
  /// The solver itself lives on the worker's thread, not here. Rebuilt and
  /// re-adopted whenever the mesh, the flow or the settings change.
  SolverWorker worker;
  solver::SimpleSettings settings;

  /// Mirrors the worker, which is the authority: it stops itself on
  /// convergence, on the iteration limit and on divergence, so a second copy
  /// of "is it running" kept here would only ever be a chance to disagree.
  bool running{false};

  /// Outer iterations between screen updates. The solver runs continuously on
  /// its own thread; this is how much work it does before handing the field
  /// back, so a small value shows the flow developing in finer steps and a
  /// large one spends less time copying.
  int iterationsPerFrame{5};
  /// Stop automatically once every residual is below this.
  double convergenceTolerance{1e-6};
  /// Stop after this many outer iterations regardless. A run that is not
  /// converging should end and say so rather than spin indefinitely.
  long long maxIterations{5000};
  bool converged{false};
  bool hitIterationLimit{false};
  /// The last run blew up. Distinct from `errorMessage` being non-empty, which
  /// also covers a solver that could not be built at all - and those need
  /// different responses, so they are tracked separately.
  bool diverged{false};

  long long iteration{0};
  solver::SolverMonitor monitor;
  /// log10 of the continuity residual, for the convergence plot.
  std::vector<float> continuityHistory;
  std::vector<float> momentumHistory;

  std::string errorMessage;
  /// Set when anything the solver was built from changes.
  bool dirty{true};
};

/// Surface distributions, streamlines, and how they are shown.
struct SurfaceState {
  bool showWallShear{true};
  bool showSeparation{true};
  bool showStreamlines{false};
  bool showSurfacePlots{true};

  std::optional<post::SurfaceDistribution> distribution;
  std::vector<post::Streamline> streamlines;

  /// Integrated from the distribution above, so it is exactly as current as
  /// the surface is - there is no second path by which a coefficient could be
  /// shown next to a Cp plot it does not belong to.
  std::optional<post::AerodynamicForces> forces;

  /// Where the pitching moment is taken about, as a fraction of chord. The
  /// quarter chord by convention: for a thin section in attached flow the
  /// aerodynamic centre sits very close to it, which makes the moment about it
  /// nearly independent of incidence.
  double momentReferenceFraction{0.25};

  /// Coefficient history, one entry per extraction, for the convergence
  /// sparklines. A force that is still moving when the residuals have stopped
  /// is not a converged force, and only a trace shows that.
  std::vector<float> liftHistory;
  std::vector<float> dragHistory;
  bool showForcePlots{true};

  /// Iteration whose coefficients were last written to the log, or -1.
  ///
  /// A run that finishes should say what it found, once. Without this it would
  /// either repeat the line on every frame after convergence or never print it
  /// at all.
  long long forcesReportedAt{-1};

  /// Plot-ready copies, built once when the distribution changes rather than
  /// rebuilt every frame.
  std::vector<float> upperX;
  std::vector<float> upperCp;
  std::vector<float> upperCf;
  std::vector<float> lowerX;
  std::vector<float> lowerCp;
  std::vector<float> lowerCf;

  int streamlineSeeds{22};
  std::string errorMessage;
  bool dirty{true};

  /// When the streamlines were last traced.
  ///
  /// Extracting the wall quantities is cheap - a walk along one row of faces -
  /// but tracing streamlines integrates thousands of RK4 steps through the
  /// mesh, and the field arrives from the solver many times a second. Retracing
  /// on every arrival would put the solver's publication rate straight onto the
  /// render thread's bill, so while a solve is running they are refreshed on a
  /// timer instead and brought fully up to date the moment it stops.
  std::chrono::steady_clock::time_point lastStreamlineTrace{};
  bool hasTracedStreamlines{false};

  /// Last separation locations written to the log, or -1 for "none". The
  /// surface is re-extracted every frame the solver advances, so without this
  /// the console would fill with one identical line per frame; a moving
  /// separation point is news, a stationary one is not.
  double reportedUpperSeparation{-1.0};
  double reportedLowerSeparation{-1.0};
};

/// An automated angle-of-attack sweep.
///
/// Driven as a state machine over frames rather than as a loop, because every
/// point is a full solve: a loop would freeze the window for the minutes the
/// sweep takes, and the flow developing at each incidence is worth watching.
/// The sweep sets the incidence, hands the existing solver the same work it
/// would do for a single point, and waits for it to stop.
struct PolarState {
  double startDeg{0.0};
  double endDeg{18.0};
  double stepDeg{2.0};

  /// Continue each point from the previous point's field instead of starting
  /// cold. Enormously faster - a degree of incidence is a small perturbation -
  /// and validated in the surface tests to reach the same answer. Left
  /// switchable because continuation is also how hysteresis would be
  /// introduced if the flow ever had more than one steady state.
  bool continueBetweenPoints{true};

  /// Where the sweep is between points. The phase and the decision made from
  /// it both live in cfd_post, so the sequencing can be tested without a
  /// window; this only holds the current value.
  post::SweepPhase phase{post::SweepPhase::Idle};

  bool running{false};
  std::size_t index{0};
  std::vector<double> angles;
  post::Polar polar;

  /// Frames spent waiting for the solver to start, so a point that never
  /// begins cannot wedge the sweep silently.
  int startupFrames{0};

  /// The current angle is being retried from the undisturbed stream after its
  /// continued solve diverged. One retry per point: a second failure is a real
  /// result about that operating point, not something to keep hammering at.
  bool retryingCold{false};

  std::array<char, 256> csvPath{"polar.csv"};
  std::string statusMessage;
  std::string errorMessage;
  /// Set once a sweep has finished and its CSV has been written.
  std::string savedPath;

  // Plot-ready copies, rebuilt whenever a point is recorded.
  std::vector<float> alphaAxis;
  std::vector<float> clSeries;
  std::vector<float> cdSeries;
  std::vector<float> cmSeries;
  std::vector<float> ldSeries;
  bool showPolarPlots{true};

  /// Incidence to restore when the sweep ends, so a sweep does not leave the
  /// session somewhere the user never chose to be.
  double restoreAngleDeg{0.0};
  bool hasRestoreAngle{false};
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
  PolarState polar;
  FlowState flow;
  SolverState solving;
  SurfaceState surface;

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

/// Re-initialise the flow and re-apply the boundary conditions if anything
/// they depend on changed. Must run after updateMesh: the field is sized by
/// the mesh and the conditions are applied to its faces.
void updateFlow(UiState& ui);

/// Rescale the field colour map to whatever the field currently holds. Must be
/// called after anything that changes it, not only after initialisation.
void refreshFieldRange(UiState& ui);

/// Rebuild the solver if needed and, while running, advance it. Must run after
/// updateFlow. Returns true if the solver wants another frame immediately,
/// which is what tells the application to keep redrawing rather than idling.
bool updateSolver(UiState& ui);

/// Re-extract the surface distributions and streamlines when the field moves.
/// Runs after updateSolver, since both are read from the solved field.
void updateSurface(UiState& ui);

/// Advance an angle-of-attack sweep. Must run after updateSurface: a point is
/// recorded from the forces that were just integrated.
void updatePolar(UiState& ui);

/// Begin a sweep over the configured range. Reports why not, if it cannot.
void startPolarSweep(UiState& ui);
/// Stop a sweep where it stands, keeping the points already gathered.
void stopPolarSweep(UiState& ui, std::string_view reason);

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
void drawFlowPanel(UiState& ui);
void drawSolverPanel(UiState& ui);
void drawSurfacePanel(UiState& ui);
void drawForcePanel(UiState& ui);
void drawPolarPanel(UiState& ui);
void drawSessionPanel(UiState& ui);
void drawViewport(UiState& ui);
void drawConsole(UiState& ui);
void drawStatusBar(UiState& ui);

/// Opens on demand; call once per frame from the top level.
void drawAboutModal(UiState& ui);

}  // namespace cfd::app
