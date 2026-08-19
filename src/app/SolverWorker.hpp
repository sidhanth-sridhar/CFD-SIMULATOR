// SolverWorker.hpp - runs the SIMPLE solver on a thread of its own.
//
// Why this exists
// ---------------
// One SIMPLE outer iteration is Gauss-Seidel sweeps for two momentum
// components, Green-Gauss gradients, Rhie-Chow face fluxes and a preconditioned
// conjugate-gradient pressure solve. On the fine C-grid - 105,410 cells,
// 211,777 faces - that is around 180 ms. Run five of those inside the frame
// callback and the window redraws once a second: panning, zooming, even
// dragging a splitter stops responding, and the application feels broken
// exactly when it is working hardest.
//
// The fix is the obvious one: the solver gets its own thread and the render
// thread never waits for it. What that costs is the one thing threads always
// cost - the field is now written by one thread and read by another, so it has
// to be handed over rather than shared.
//
// The handover
// ------------
// The worker never lets the UI see its live field. It iterates on its own copy,
// and every so often builds a `SolverUpdate` - a complete, self-consistent
// snapshot - and leaves it under a mutex for the UI to collect. The UI moves it
// out, so the only cost on the render thread is a pointer swap; the copy itself
// happens on the worker.
//
// Snapshots that are never collected are coalesced: the field is simply
// overwritten by the newer one. The residual history is not, because dropping
// entries there would silently put holes in the convergence plot, so it
// accumulates until it is collected.
//
// Mesh lifetime
// -------------
// The solver holds a bare pointer to the mesh it was built on, and the UI
// thread is free to regenerate that mesh at any moment. The worker therefore
// holds a shared_ptr to the grid for as long as it might touch it: a
// regeneration on the UI thread swaps its own pointer and the old grid stays
// alive until the worker is done with it.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "cfd/flow/FlowField.hpp"
#include "cfd/mesh/Mesh.hpp"
#include "cfd/solver/SimpleSolver.hpp"

namespace cfd::app {

/// One self-consistent snapshot of a run, as handed to the UI.
struct SolverUpdate {
  flow::FlowField field;
  std::vector<double> divergence;
  solver::SolverMonitor monitor;
  long long iteration{0};

  /// log10 of the residuals, one entry per iteration since the previous
  /// collection. Per-iteration rather than per-snapshot so the convergence plot
  /// keeps its shape however rarely the UI polls.
  std::vector<float> continuityHistory;
  std::vector<float> momentumHistory;

  bool converged{false};
  bool hitIterationLimit{false};
  /// The residuals stopped being finite. The run is over and the field is
  /// whatever it had reached, which is worth nothing.
  bool diverged{false};
  /// Whether the worker was still iterating when this snapshot was taken.
  bool running{false};

  /// Turbulence diagnostics, zero without a model. y+ decides whether the wall
  /// treatment is being used inside its range of validity, and mu_t/mu says
  /// whether the model is doing anything at all.
  double maxEddyViscosityRatio{0.0};
  double wallYPlus{0.0};

  /// The model's own fields, so the viewport can shade by them. Empty without a
  /// model, which is what tells the UI those views have nothing behind them.
  std::vector<double> turbulentEnergy;
  std::vector<double> dissipation;
  std::vector<double> eddyViscosity;
};

/// Drives a SimpleSolver on a background thread.
///
/// Every public function is safe to call from the UI thread. None of them
/// block on an iteration finishing except `stop()`, which has to.
class SolverWorker {
 public:
  SolverWorker() = default;
  ~SolverWorker();

  SolverWorker(const SolverWorker&) = delete;
  SolverWorker& operator=(const SolverWorker&) = delete;
  SolverWorker(SolverWorker&&) = delete;
  SolverWorker& operator=(SolverWorker&&) = delete;

  /// Take over a solver and the grid it was built on. Stops and joins any run
  /// already in progress, so the previous solver is destroyed before the new
  /// one starts.
  void adopt(std::shared_ptr<const mesh::Mesh> mesh, solver::SimpleSolver solver);

  /// Stop iterating and join the thread. Safe when nothing is running, and
  /// safe to call twice.
  void stop();

  [[nodiscard]] bool hasSolver() const;

  /// Start or pause. The worker is the authority on whether it is running - it
  /// stops itself on convergence, on the iteration limit and on divergence -
  /// so the UI mirrors this rather than tracking its own copy.
  void setRunning(bool running);
  [[nodiscard]] bool isRunning() const noexcept { return running_.load(); }

  /// Run exactly `count` more iterations and then pause.
  void requestIterations(long long count);

  void setSettings(const solver::SimpleSettings& settings);

  /// Stop conditions and how often to publish.
  ///
  /// `iterationsPerUpdate` is a *minimum*: a snapshot also waits for a short
  /// interval to pass, so a cheap mesh cannot flood the UI with updates it has
  /// no way to display.
  void setLimits(double convergenceTolerance, long long maxIterations,
                 int iterationsPerUpdate);

  /// Collect the newest snapshot. Returns false if none arrived since the last
  /// call, in which case `out` is untouched.
  [[nodiscard]] bool poll(SolverUpdate& out);

 private:
  void runLoop();
  /// True while the worker should keep iterating. Called with the lock held.
  [[nodiscard]] bool shouldIterate() const;

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::thread thread_;

  // --- owned by the worker thread once started ---
  /// Declared before the solver so it is destroyed *after* it: the solver
  /// points into this grid.
  std::shared_ptr<const mesh::Mesh> mesh_;
  std::optional<solver::SimpleSolver> solver_;

  // --- control, guarded by mutex_ ---
  bool quit_{false};
  /// Remaining iterations to run, or -1 for "keep going".
  long long budget_{-1};
  solver::SimpleSettings settings_;
  double tolerance_{1e-6};
  long long maxIterations_{5000};
  int iterationsPerUpdate_{5};

  /// Read without the lock by isRunning(), which the UI calls every frame.
  std::atomic<bool> running_{false};

  // --- publication, guarded by mutex_ ---
  SolverUpdate pending_;
  bool hasPending_{false};
  /// Kept outside `pending_` so that a coalesced snapshot does not drop the
  /// iterations the UI has not seen yet.
  std::vector<float> pendingContinuity_;
  std::vector<float> pendingMomentum_;
};

}  // namespace cfd::app
