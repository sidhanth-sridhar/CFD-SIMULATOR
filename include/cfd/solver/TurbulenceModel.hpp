// TurbulenceModel.hpp - the closure the averaged equations cannot supply.
//
// Why averaging leaves a hole
// ---------------------------
// Turbulent flow is unsteady and three-dimensional at every scale down to
// millimetres, and resolving all of it is out of reach for anything but the
// smallest problems. So the equations are *averaged*: every quantity is split
// into a mean and a fluctuation, u = U + u', and the Navier-Stokes equations
// are averaged over the fluctuations.
//
// Almost every term survives that operation unchanged, because averaging is
// linear and most of the terms are linear in u. The exception is convection,
// which is quadratic. Averaging div(rho u u) gives
//
//     div(rho U U)  +  div(rho <u' u'>)
//
// The first term is what the laminar solver already assembles. The second is
// new: the mean momentum is being transported by the fluctuations, and it acts
// on the mean flow exactly as an extra stress would. Those are the **Reynolds
// stresses**, and they are the entire difficulty.
//
//     div(rho U U) = -grad(P) + div(mu grad(U)) - div(rho <u' u'>)
//
// The problem is that nothing determines them. Averaging the equations gave one
// new unknown per stress component and no new equations, and deriving a
// transport equation for <u'u'> introduces triple correlations <u'u'u'>, and so
// on forever. This is the **closure problem**: the averaged equations are not a
// closed system, and no amount of manipulation makes them one.
//
// A turbulence model is a *guess* at the Reynolds stresses in terms of
// quantities the mean flow already has. That is why there are dozens of them,
// why none is universally right, and why the choice of model is a modelling
// decision rather than a numerical one.
//
// The eddy-viscosity idea
// -----------------------
// The most widely used guess is Boussinesq's: that turbulence mixes momentum
// the way molecular motion does, only far more strongly. If so, the Reynolds
// stresses can be written like a viscous stress with a larger coefficient,
//
//     -rho <u' u'> = mu_t (grad U + grad U^T) - (2/3) rho k I
//
// with mu_t the **eddy viscosity** - a property of the flow, not the fluid,
// varying by orders of magnitude across a boundary layer. What a model of this
// family actually does is compute mu_t.
//
// That is what makes this interface small, and it is why the momentum assembly
// needs no changes at all: it already reads a per-cell effective viscosity, so
// a model's whole job is to keep that field up to date with mu + mu_t.
//
// What this interface deliberately does not assume
// -----------------------------------------------
// It does not mention k, omega, epsilon or any other particular variable. A
// model owns whatever transport equations it needs and exposes only the eddy
// viscosity and its own residuals. Swapping k-omega SST for Spalart-Allmaras or
// a mixing length has to be possible without the solver knowing.

#pragma once

#include <algorithm>
#include <memory>
#include <string_view>
#include <vector>

#include "cfd/core/Error.hpp"
#include "cfd/core/Vec2.hpp"
#include "cfd/flow/BoundaryConditions.hpp"
#include "cfd/flow/FlowField.hpp"
#include "cfd/mesh/Mesh.hpp"

namespace cfd::solver {

/// Freestream turbulence, as the quantities a user can actually state.
///
/// Nobody knows the freestream k and omega of their case; what they know is how
/// turbulent the oncoming stream is and how strongly it is already mixing.
/// Models convert these into whatever variables they use.
struct TurbulenceInflow {
  /// Turbulence intensity, u'/U. 0.001 is a good wind tunnel, 0.05 an
  /// industrial one.
  double intensity{0.001};
  /// Ratio of eddy to molecular viscosity in the freestream. External
  /// aerodynamics conventionally uses something between 1 and 10; the point of
  /// a small value is that the freestream should not be doing the mixing.
  double viscosityRatio{1.0};

  [[nodiscard]] Status validate() const;
};

/// Everything a model needs from the mean flow to take one step.
///
/// Passed by reference each outer iteration rather than stored, so a model
/// cannot quietly hold a pointer into solver state that later moves.
struct TurbulenceContext {
  const mesh::Mesh* mesh{nullptr};
  const flow::FlowField* field{nullptr};
  /// Boundary conditions per face. Handed over every iteration rather than
  /// stored by the model, because SimpleSolver is movable: a model that cached
  /// a pointer to the solver's own copy would be left pointing at a moved-from
  /// vector the moment the solver was handed to a worker thread.
  const flow::FaceConditions* conditions{nullptr};
  /// Mass flux through each face, positive out of the owner. The model's own
  /// transport equations are convected by the same fluxes as the momentum
  /// equations, which is what keeps the two consistent.
  const std::vector<double>* massFlux{nullptr};
  /// Mean velocity gradients, per cell.
  const std::vector<Vec2>* gradU{nullptr};
  const std::vector<Vec2>* gradV{nullptr};
  /// Molecular (not effective) viscosity, per cell.
  const std::vector<double>* molecularViscosity{nullptr};
  /// Under-relaxation for the model's own equations.
  double relaxation{0.7};
};

/// How far a model's own equations are from being satisfied.
struct TurbulenceResiduals {
  double first{0.0};   ///< normalised residual of the model's first equation
  double second{0.0};  ///< and of its second, if it has one

  [[nodiscard]] double worst() const noexcept { return std::max(first, second); }
};

/// A closure for the Reynolds stresses.
///
/// Implementations own their transport equations, their wall treatment and
/// their constants. The solver knows only that something produces an eddy
/// viscosity and can be asked to advance.
class TurbulenceModel {
 public:
  TurbulenceModel() = default;
  virtual ~TurbulenceModel() = default;

  TurbulenceModel(const TurbulenceModel&) = delete;
  TurbulenceModel& operator=(const TurbulenceModel&) = delete;
  TurbulenceModel(TurbulenceModel&&) = delete;
  TurbulenceModel& operator=(TurbulenceModel&&) = delete;

  /// What to call this in a log line or a panel.
  [[nodiscard]] virtual std::string_view name() const noexcept = 0;

  /// Prepare for a run on this mesh, with these boundary conditions and this
  /// starting field. Anything expensive and fixed - wall distances, say -
  /// belongs here rather than in update().
  [[nodiscard]] virtual Status initialise(const mesh::Mesh& mesh,
                                          const flow::FaceConditions& conditions,
                                          const flow::FlowField& field,
                                          const TurbulenceInflow& inflow) = 0;

  /// Advance the model's own equations by one outer iteration and refresh the
  /// eddy viscosity.
  virtual void update(const TurbulenceContext& context) = 0;

  /// Eddy viscosity per cell, in Pa.s. Valid after initialise().
  [[nodiscard]] virtual const std::vector<double>& eddyViscosity() const noexcept = 0;

  [[nodiscard]] virtual TurbulenceResiduals residuals() const noexcept = 0;

  /// Largest y+ over the wall faces, or 0 if the model does not track it.
  ///
  /// Not decoration: a low-Reynolds wall treatment is only valid where the
  /// first cell sits inside the viscous sublayer, and a model that is being
  /// used outside its range of validity should be able to say so.
  [[nodiscard]] virtual double maxWallYPlus() const noexcept { return 0.0; }
};

/// Distance from every cell centroid to the nearest no-slip wall face.
///
/// Every wall-damped turbulence model needs this, and it is a property of the
/// mesh and the boundary conditions rather than of any one model, so it lives
/// here where it can be tested on its own.
///
/// Computed by brute force over the wall faces. That is O(cells x walls) - 67
/// million distance evaluations on the fine C-grid - but it runs once per
/// solve, and a search structure would be a lot of machinery to save a fraction
/// of a second against solves that take minutes.
[[nodiscard]] std::vector<double> wallDistances(const mesh::Mesh& mesh,
                                                const flow::FaceConditions& conditions);

/// A model that returns no eddy viscosity at all.
///
/// Not a placeholder: "the flow is laminar" is a perfectly good closure, and
/// having it behind the same interface is what lets the solver treat laminar
/// and turbulent runs identically instead of branching on whether a model
/// exists. It is also the control case for every turbulence test - if a result
/// changes when this is substituted, the change is the model's doing.
class LaminarModel final : public TurbulenceModel {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "laminar"; }

  [[nodiscard]] Status initialise(const mesh::Mesh& mesh,
                                  const flow::FaceConditions& conditions,
                                  const flow::FlowField& field,
                                  const TurbulenceInflow& inflow) override;

  void update(const TurbulenceContext& context) override;

  [[nodiscard]] const std::vector<double>& eddyViscosity() const noexcept override {
    return eddyViscosity_;
  }

  [[nodiscard]] TurbulenceResiduals residuals() const noexcept override { return {}; }

 private:
  std::vector<double> eddyViscosity_;
};

}  // namespace cfd::solver
