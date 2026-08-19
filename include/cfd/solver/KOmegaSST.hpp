// KOmegaSST.hpp - Menter's shear-stress transport model.
//
// The two quantities
// ------------------
// An eddy viscosity has dimensions of density x velocity x length, so a model
// that computes one needs a velocity scale and a length scale for the
// turbulence. This family gets them from two transported quantities:
//
//   k      turbulent kinetic energy, (1/2)<u'.u'>, in m^2/s^2. The energy in
//          the fluctuations, so sqrt(k) is their velocity scale.
//   omega  specific dissipation rate, in 1/s. How fast that energy is handed
//          down to smaller scales, per unit of it. Its reciprocal is the
//          lifetime of an eddy, so sqrt(k)/omega is a length.
//
// Together mu_t = rho k / omega, up to a limiter discussed below.
//
// Why *two* models blended
// ------------------------
// The k-omega model of Wilcox behaves beautifully near a wall - it integrates
// straight through the viscous sublayer with no damping functions - and badly
// in the freestream, where the answer depends alarmingly on the omega you
// happened to specify at the inlet. The k-epsilon model is the other way round:
// robust in free shear, and needing damping functions to survive near a wall.
//
// Menter's insight was that you can have both. Rewrite k-epsilon in terms of
// omega and it differs from k-omega by exactly one extra term - the
// **cross-diffusion** term - so the two can be blended by switching that term
// on and off, along with the constants. A blending function F1 is built to be 1
// inside the boundary layer and 0 outside it, from quantities that measure
// distance to the wall in turbulence units. The result uses k-omega where
// k-omega is good and k-epsilon where k-epsilon is.
//
// The SST limiter
// ---------------
// The second idea, and the one the model is named for. Standard eddy-viscosity
// models over-predict mu_t in adverse pressure gradients, which makes boundary
// layers far too reluctant to separate - the single most damaging error for
// aerofoil work. Bradshaw's observation is that in a boundary layer the shear
// stress is close to a constant times k. Enforcing that as a *limit*,
//
//     mu_t = rho a1 k / max(a1 omega, S F2)
//
// caps mu_t exactly where the strain rate is high, restoring the separation
// behaviour. That is what "shear stress transport" refers to.
//
// The equations, exactly as implemented
// -------------------------------------
// Steady, incompressible, two dimensions. S is the strain-rate invariant
// sqrt(2 S_ij S_ij) with S_ij = (1/2)(du_i/dx_j + du_j/dx_i), and d is the
// distance to the nearest wall.
//
//   Turbulent kinetic energy
//     div(rho U k) = P~_k - beta* rho omega k + div[(mu + sigma_k mu_t) grad k]
//
//   Specific dissipation rate
//     div(rho U omega) = gamma rho S^2 - beta rho omega^2
//                        + div[(mu + sigma_w mu_t) grad omega]
//                        + 2(1 - F1) rho sigma_w2 (1/omega) grad k . grad omega
//
//   Production, with Menter's limiter
//     P_k  = mu_t S^2
//     P~_k = min(P_k, 10 beta* rho k omega)
//
//   Eddy viscosity, with the shear-stress limiter
//     mu_t = rho a1 k / max(a1 omega, S F2)
//
//   Blending
//     F1 = tanh(arg1^4),  arg1 = min( max( sqrt(k)/(beta* omega d),
//                                          500 nu/(d^2 omega) ),
//                                     4 rho sigma_w2 k / (CD_kw d^2) )
//     F2 = tanh(arg2^2),  arg2 = max( 2 sqrt(k)/(beta* omega d),
//                                     500 nu/(d^2 omega) )
//     CD_kw = max( 2 rho sigma_w2 (1/omega) grad k . grad omega, 1e-10 )
//
//   Blended constants, phi = F1 phi_1 + (1 - F1) phi_2
//     sigma_k1 = 0.85   sigma_w1 = 0.5    beta_1 = 0.075
//     sigma_k2 = 1.0    sigma_w2 = 0.856  beta_2 = 0.0828
//     beta* = 0.09      kappa = 0.41      a1 = 0.31
//     gamma_i = beta_i/beta* - sigma_wi kappa^2 / sqrt(beta*)
//
//   Wall treatment, at the first cell
//     k     : zero imposed on the wall *face*; the cell value is solved
//     omega : sqrt(omega_vis^2 + omega_log^2) imposed on the cell, with
//             omega_vis = 6 nu/(beta_1 d^2) and
//             omega_log = sqrt(k)/(beta*^(1/4) kappa d)
//
// Simplifications, stated rather than hidden
// ------------------------------------------
// 1. **The -(2/3) rho k I part of the Boussinesq stress is not added to the
//    momentum equation.** It is isotropic, so for constant density it is a
//    gradient of a scalar and can be absorbed into the pressure. What the solver
//    reports as pressure is therefore p + (2/3) rho k. At the turbulence levels
//    here that is under a per cent of the dynamic pressure, but it is a
//    modelling choice and Cp inherits it.
// 2. **The omega production uses gamma rho S^2 unlimited**, while the k
//    production is limited. Substituting P_k = mu_t S^2 into the exact
//    (gamma/nu_t) P_k gives gamma rho S^2 identically, so this is exact for the
//    unlimited term; the choice is whether the *limiter* also applies here.
//    Menter states it for the k equation, and this follows that. Some codes
//    limit both, which makes them slightly more diffusive at a stagnation point.
// 3. **Convection of k and omega is first-order upwind**, where the momentum
//    equations offer a second-order option. Deliberate: boundedness matters more
//    than accuracy for strictly positive quantities, and an overshoot that
//    drives k negative is not a small error.
// 4. **No transition model.** The boundary layer is turbulent from the leading
//    edge. Real flows at Re = 10^6 have a laminar run first, so drag is
//    over-predicted and the effect is largest at low incidence.
// 5. **No compressibility, buoyancy or rotation/curvature corrections.** None
//    apply to steady incompressible two-dimensional flow over a fixed section.
//
// Numerical treatment
// -------------------
// * **Positivity of k** - destruction beta* rho omega k is written as
//   (beta* rho omega) * k so it lands on the diagonal, where a larger k makes
//   its own sink larger. Production is explicit and positive. k is floored at
//   zero after each solve.
// * **Positivity of omega** - same split, beta rho omega^2 as (beta rho omega)
//   * omega. omega is floored at 1e-10 because it appears in denominators
//   throughout, and a transported quantity that is only *physically* positive
//   will go negative somewhere on a real mesh during an iteration.
// * **Stiff sources** - the implicit destruction above is what makes the source
//   terms tractable; treating them explicitly requires an impractically small
//   relaxation. The equations are additionally under-relaxed at 0.5 by default,
//   lower than the momentum value.
// * **Near-wall behaviour** - no wall function. The model integrates to the wall
//   and needs y+ of order 1; it reports the y+ it achieved so being used outside
//   that range is visible rather than silent.
//
// What this implementation is and is not
// --------------------------------------
// It is the 2003 form of the model with the standard constants, the production
// limiter, and Menter's low-Reynolds wall treatment for omega.

#pragma once

#include <vector>

#include "cfd/solver/TurbulenceModel.hpp"

namespace cfd::solver {

/// The model's constants, exposed because they are the model.
///
/// Every one of these is calibrated rather than derived, which is worth being
/// able to see: they come from matching decaying grid turbulence, the log law
/// and a handful of free shear flows, and changing one is changing the model.
struct SSTConstants {
  // Set 1, used near the wall (the k-omega end).
  double sigmaK1{0.85};
  double sigmaOmega1{0.5};
  double beta1{0.075};

  // Set 2, used away from it (the k-epsilon end).
  double sigmaK2{1.0};
  double sigmaOmega2{0.856};
  double beta2{0.0828};

  double betaStar{0.09};
  double kappa{0.41};
  /// Bradshaw's constant, the shear-stress limiter.
  double a1{0.31};
  /// Cap on production as a multiple of dissipation, which stops k running away
  /// at a stagnation point - where the strain rate is large and the physical
  /// production is not.
  double productionLimit{10.0};

  /// gamma_i = beta_i/beta* - sigma_omega_i kappa^2 / sqrt(beta*), the
  /// coefficient that makes the model reproduce the log law.
  [[nodiscard]] double gamma1() const noexcept;
  [[nodiscard]] double gamma2() const noexcept;
};

/// k-omega SST.
class KOmegaSST final : public TurbulenceModel {
 public:
  KOmegaSST() = default;
  explicit KOmegaSST(const SSTConstants& constants) : constants_(constants) {}

  [[nodiscard]] std::string_view name() const noexcept override { return "k-omega SST"; }

  [[nodiscard]] Status initialise(const mesh::Mesh& mesh,
                                  const flow::FaceConditions& conditions,
                                  const flow::FlowField& field,
                                  const TurbulenceInflow& inflow) override;

  void update(const TurbulenceContext& context) override;

  [[nodiscard]] const std::vector<double>& eddyViscosity() const noexcept override {
    return eddyViscosity_;
  }

  [[nodiscard]] TurbulenceResiduals residuals() const noexcept override {
    return residuals_;
  }

  [[nodiscard]] double maxWallYPlus() const noexcept override { return maxYPlus_; }
  [[nodiscard]] TurbulenceRanges ranges() const noexcept override { return ranges_; }

  // --- inspection, for tests and for the panel ---
  [[nodiscard]] const std::vector<double>& turbulentEnergy() const noexcept { return k_; }
  [[nodiscard]] const std::vector<double>& specificDissipation() const noexcept {
    return omega_;
  }
  [[nodiscard]] const std::vector<double>& wallDistance() const noexcept {
    return wallDistance_;
  }
  /// The blending function, 1 near a wall and 0 in free stream.
  [[nodiscard]] const std::vector<double>& blending() const noexcept { return f1_; }
  /// Cross-diffusion as it enters the omega equation: signed, unclipped.
  [[nodiscard]] const std::vector<double>& crossDiffusion() const noexcept {
    return crossDiffusion_;
  }
  [[nodiscard]] double freestreamEnergy() const noexcept { return kInflow_; }
  [[nodiscard]] double freestreamDissipation() const noexcept { return omegaInflow_; }

 private:
  /// Strain-rate magnitude sqrt(2 S_ij S_ij) and vorticity, per cell.
  void computeStrain(const TurbulenceContext& context);
  void computeBlending(const TurbulenceContext& context);
  void solveTransport(const TurbulenceContext& context);
  void updateEddyViscosity(const TurbulenceContext& context);
  void applyWallConditions(const TurbulenceContext& context);
  void updateRanges();

  SSTConstants constants_{};
  const mesh::Mesh* mesh_{nullptr};

  std::vector<double> k_;
  std::vector<double> omega_;
  std::vector<double> eddyViscosity_;
  std::vector<double> wallDistance_;

  // Working arrays, kept between iterations so they are allocated once.
  std::vector<double> strain_;
  std::vector<double> f1_;
  std::vector<double> f2_;
  /// Cross-diffusion, signed - as the omega equation needs it.
  std::vector<double> crossDiffusion_;
  /// The same quantity floored at 1e-10, as F1's argument needs it. Kept
  /// separately because clipping the source term deletes real physics.
  std::vector<double> crossDiffusionPositive_;
  std::vector<Vec2> gradK_;
  std::vector<Vec2> gradOmega_;
  std::vector<double> production_;

  double kInflow_{0.0};
  double omegaInflow_{0.0};
  double maxYPlus_{0.0};
  TurbulenceResiduals residuals_{};
  TurbulenceRanges ranges_{};
};

}  // namespace cfd::solver
