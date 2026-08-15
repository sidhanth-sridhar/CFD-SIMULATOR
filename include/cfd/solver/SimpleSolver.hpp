// SimpleSolver.hpp - steady incompressible Navier-Stokes by the SIMPLE algorithm.
//
// The equations
// -------------
// Steady, constant density, laminar:
//
//     continuity:  div(u) = 0
//     momentum:    div(rho u u) = -grad(p) + div(mu grad(u))
//
// Read the momentum equation as a balance of three effects on a lump of fluid:
// convection carries momentum with the flow, the pressure gradient pushes it,
// and viscosity diffuses it from fast fluid into slow. Everything this solver
// does is an attempt to satisfy those two statements on every cell at once.
//
// The difficulty: pressure has no equation
// ----------------------------------------
// Momentum tells you how to advance velocity given a pressure field.
// Continuity does not contain pressure at all - it is a *constraint* on
// velocity. So there is no equation to "solve for pressure", and the two are
// tangled: you need p to get u, and you need u to know whether p was right.
//
// SIMPLE (Semi-Implicit Method for Pressure-Linked Equations) breaks the
// tangle by guessing and correcting:
//
//   1. Guess a pressure field and solve momentum with it. The resulting
//      velocity satisfies momentum but not continuity.
//   2. Ask what pressure *correction* would fix the leftover mass imbalance.
//      Substituting the correction into momentum and then into continuity
//      gives a Poisson-like equation for it.
//   3. Correct pressure, velocity and the face fluxes, and repeat.
//
// Each pass is called an outer iteration. It is not a time step - nothing
// physical happens between iterations, they simply walk towards the steady
// answer - which is why convergence is judged by residuals rather than by
// having reached some time.
//
// Under-relaxation
// ----------------
// Step 2 deliberately drops a term (the effect of neighbouring corrections),
// so the correction it produces is too large. Applied in full it oscillates
// and diverges. Both equations are therefore under-relaxed: only a fraction of
// each update is kept. That is why 0.7 and 0.3 appear below, and it is why
// SIMPLE takes hundreds of iterations rather than tens.
//
// Collocated storage and checkerboarding
// --------------------------------------
// Velocity and pressure both live at cell centres, which is what makes the
// mesh unstructured-capable. It also opens a notorious failure mode: a naive
// face interpolation makes each cell's pressure gradient depend only on its
// *second* neighbours, so a pressure field alternating cell by cell looks
// perfectly uniform to the discretisation. The solver then happily returns a
// checkerboard. Rhie-Chow interpolation is the fix: the face flux is built
// with a compact pressure difference across that face, restoring the coupling
// that the interpolation lost.

#pragma once

#include <cstddef>
#include <vector>

#include "cfd/core/Error.hpp"
#include "cfd/core/Vec2.hpp"
#include "cfd/flow/BoundaryConditions.hpp"
#include "cfd/flow/FlowField.hpp"
#include "cfd/mesh/Mesh.hpp"
#include "cfd/solver/LinearSystem.hpp"

namespace cfd::solver {

/// How the convected value at a face is estimated from the cells either side.
enum class ConvectionScheme {
  /// Take the value from whichever cell is upstream. Unconditionally stable
  /// and never overshoots, but only first-order accurate: the error behaves
  /// like an extra diffusion aligned with the flow, which smears gradients.
  Upwind,
  /// Upwind plus a deferred-correction term towards a central value, giving
  /// second-order accuracy where the field is smooth. More accurate, less
  /// robust - the smearing that upwind adds is also what steadies it.
  SecondOrderUpwind,
};

[[nodiscard]] std::string_view toString(ConvectionScheme scheme) noexcept;

struct SimpleSettings {
  /// Fraction of each momentum update kept. Applied implicitly, by inflating
  /// the diagonal, which is more stable than scaling the update afterwards.
  ///
  /// The textbook pair is 0.7 with 0.3 below, and that is what the Cartesian
  /// validation cases run at happily. The defaults here are deliberately more
  /// cautious: on the aerofoil C-grid, where non-orthogonality reaches 75
  /// degrees and cells near the wake are thousands of times longer than they
  /// are thick, 0.7 diverges within a hundred iterations. Converging slowly is
  /// recoverable; diverging is not.
  double velocityRelaxation{0.5};
  /// Fraction of the pressure correction kept. Lower than the velocity value
  /// because the correction is the quantity SIMPLE approximates most crudely.
  double pressureRelaxation{0.2};

  /// Gauss-Seidel sweeps per momentum equation. Solving these tightly is
  /// wasted work: the pressure is about to change anyway.
  int momentumSweeps{3};
  /// Cap on conjugate-gradient iterations for the pressure correction.
  int pressureIterations{200};
  /// Relative tolerance for the pressure solve within one outer iteration.
  double pressureTolerance{0.01};

  ConvectionScheme scheme{ConvectionScheme::Upwind};

  /// Extra passes of the pressure equation that re-evaluate the
  /// non-orthogonal part of the flux. Needed on skewed meshes; the C-grid
  /// reaches 75 degrees of non-orthogonality at the trailing edge.
  int nonOrthogonalCorrectors{1};

  /// How much of the non-orthogonal diffusion correction to apply, 0 to 1.
  ///
  /// The correction is explicit - it uses the previous iteration's gradient -
  /// so on a badly skewed mesh it can feed back on itself and diverge. Damping
  /// it trades a little accuracy on skewed cells for the ability to converge at
  /// all. 1 is correct where the mesh allows it.
  double nonOrthogonalBlend{1.0};
};

/// What one outer iteration achieved.
struct SolverMonitor {
  flow::ResidualSet residuals;
  /// Sum over cells of the absolute net mass flux, in kg/(m s). Zero for a
  /// converged incompressible solution.
  double massImbalance{0.0};
  /// Largest net volume flux per unit area, 1/s.
  double maxDivergence{0.0};
  /// Conjugate-gradient iterations the pressure equation needed.
  int pressureIterations{0};
};

/// Steady laminar solver.
///
/// Holds a reference to the mesh, which must outlive it.
class SimpleSolver {
 public:
  [[nodiscard]] static Result<SimpleSolver> create(const mesh::Mesh& mesh,
                                                   flow::FaceConditions conditions,
                                                   const SimpleSettings& settings);

  /// Adopt a starting field. Also seeds the face mass fluxes from it, which
  /// are a primary unknown in a collocated method rather than something
  /// derived from the cell velocities on demand.
  [[nodiscard]] Status initialise(const flow::FlowField& initial);

  /// Run one outer iteration.
  ///
  /// The residuals returned are measured *before* this iteration's updates, so
  /// a value of zero means the field it started from already satisfied the
  /// discrete equations.
  SolverMonitor iterate();

  [[nodiscard]] const flow::FlowField& field() const noexcept { return field_; }
  [[nodiscard]] flow::FlowField& field() noexcept { return field_; }
  /// Mass flux through each face, positive out of the owner, kg/(m s).
  [[nodiscard]] const std::vector<double>& massFlux() const noexcept { return massFlux_; }
  [[nodiscard]] const SimpleSettings& settings() const noexcept { return settings_; }
  void setSettings(const SimpleSettings& settings) { settings_ = settings; }

  /// Net volume flux per unit area in each cell, 1/s.
  [[nodiscard]] std::vector<double> divergence() const;

 private:
  explicit SimpleSolver(const mesh::Mesh& mesh);

  /// Geometry that never changes, worked out once.
  struct FaceGeometry {
    Vec2 area{};         ///< S = n * A, out of the owner
    Vec2 delta{};        ///< d, owner centroid to neighbour centroid (or to the face)
    Vec2 tangential{};   ///< T = S - E, the part of S not along d
    double diffusion{0.0};  ///< |S|^2 / (S . d), the over-relaxed orthogonal factor
    double ownerWeight{1.0};
  };

  void precomputeGeometry();
  /// Is this face's velocity imposed, given the current flux direction?
  [[nodiscard]] bool imposesVelocity(std::size_t face) const;
  [[nodiscard]] Vec2 boundaryVelocity(std::size_t face) const;

  void interpolateFaceValues();
  void computeGradients();
  void assembleMomentum(int component);
  void applyRelaxation(int component);
  void computeRhieChowFluxes();
  void assemblePressureCorrection();
  void correct(const std::vector<double>& pressureCorrection);

  const mesh::Mesh* mesh_{nullptr};
  flow::FaceConditions conditions_;
  SimpleSettings settings_;

  flow::FlowField field_;
  std::vector<double> massFlux_;

  std::vector<FaceGeometry> geometry_;
  std::vector<double> faceU_;
  std::vector<double> faceV_;
  std::vector<double> faceP_;
  std::vector<Vec2> gradU_;
  std::vector<Vec2> gradV_;
  std::vector<Vec2> gradP_;

  LinearSystem momentum_;
  LinearSystem pressure_;
  /// Relaxed momentum diagonal, shared by both components and reused by
  /// Rhie-Chow and by the pressure equation.
  std::vector<double> momentumDiagonal_;
  std::vector<double> netFlux_;

  /// True when at least one boundary face fixes a pressure. Without one the
  /// pressure level is arbitrary and has to be handled explicitly.
  bool hasPressureReference_{false};
  double referenceMassFlow_{1.0};
};

}  // namespace cfd::solver
