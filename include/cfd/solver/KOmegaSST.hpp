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
// What this implementation is and is not
// --------------------------------------
// It is the 2003 form of the model with the standard constants, the production
// limiter, and Menter's low-Reynolds wall treatment for omega. It requires a
// mesh resolved to y+ of order 1 - there is no wall function - and reports the
// y+ it actually achieved so that being used outside that range is visible
// rather than silent.

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
  [[nodiscard]] double freestreamEnergy() const noexcept { return kInflow_; }
  [[nodiscard]] double freestreamDissipation() const noexcept { return omegaInflow_; }

 private:
  /// Strain-rate magnitude sqrt(2 S_ij S_ij) and vorticity, per cell.
  void computeStrain(const TurbulenceContext& context);
  void computeBlending(const TurbulenceContext& context);
  void solveTransport(const TurbulenceContext& context);
  void updateEddyViscosity(const TurbulenceContext& context);
  void applyWallConditions(const TurbulenceContext& context);

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
  std::vector<double> crossDiffusion_;
  std::vector<Vec2> gradK_;
  std::vector<Vec2> gradOmega_;
  std::vector<double> production_;

  double kInflow_{0.0};
  double omegaInflow_{0.0};
  double maxYPlus_{0.0};
  TurbulenceResiduals residuals_{};
};

}  // namespace cfd::solver
