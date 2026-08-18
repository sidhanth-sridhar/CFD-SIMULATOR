#include "cfd/solver/KOmegaSST.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "cfd/solver/Gradient.hpp"
#include "cfd/solver/LinearSystem.hpp"

namespace cfd::solver {
namespace {

/// Smallest omega allowed anywhere.
///
/// omega appears in denominators throughout - in mu_t, in the blending
/// functions, in the cross-diffusion term - and a transported quantity that is
/// only *physically* positive will go negative somewhere during an iteration on
/// a real mesh. Clipping is not a fudge here; it is what keeps a positive
/// quantity positive under a discretisation that does not guarantee it.
constexpr double kMinOmega = 1.0e-10;
constexpr double kMinEnergy = 0.0;
constexpr double kTiny = 1.0e-30;

/// Face value of a cell field, by the same distance weighting the momentum
/// equations use. Wall and inflow faces take the value imposed there.
void interpolate(const mesh::Mesh& mesh, const flow::FaceConditions& conditions,
                 const std::vector<double>& cellValues, double wallValue,
                 double inflowValue, const std::vector<double>& massFlux,
                 std::vector<double>& faceValues) {
  faceValues.assign(mesh.faceCount(), 0.0);
  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    const mesh::Face& face = mesh.faces()[f];
    const auto owner = static_cast<std::size_t>(face.owner);

    if (face.neighbour >= 0) {
      const double w = ownerWeight(mesh, f);
      faceValues[f] =
          w * cellValues[owner] + (1.0 - w) * cellValues[static_cast<std::size_t>(face.neighbour)];
      continue;
    }

    const int across = mesh::oppositeCell(mesh, f);
    if (across >= 0) {
      const double w = ownerWeight(mesh, f);
      faceValues[f] =
          w * cellValues[owner] + (1.0 - w) * cellValues[static_cast<std::size_t>(across)];
      continue;
    }

    switch (conditions[f].kind) {
      case flow::BoundaryKind::NoSlipWall:
        faceValues[f] = wallValue;
        break;
      case flow::BoundaryKind::Inlet:
        faceValues[f] = inflowValue;
        break;
      case flow::BoundaryKind::FarField:
        // Entering fluid brings the freestream state with it; leaving fluid
        // takes whatever the interior has, which is a zero-gradient condition.
        faceValues[f] = (massFlux[f] < 0.0) ? inflowValue : cellValues[owner];
        break;
      case flow::BoundaryKind::Outlet:
      case flow::BoundaryKind::Internal:
        faceValues[f] = cellValues[owner];
        break;
    }
  }
}

}  // namespace

double SSTConstants::gamma1() const noexcept {
  return beta1 / betaStar - sigmaOmega1 * kappa * kappa / std::sqrt(betaStar);
}

double SSTConstants::gamma2() const noexcept {
  return beta2 / betaStar - sigmaOmega2 * kappa * kappa / std::sqrt(betaStar);
}

Status KOmegaSST::initialise(const mesh::Mesh& mesh, const flow::FaceConditions& conditions,
                             const flow::FlowField& field, const TurbulenceInflow& inflow) {
  if (const Status valid = inflow.validate(); !valid) {
    return valid.error();
  }
  if (field.size() != mesh.cellCount()) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("field has {} cells but the mesh has {}", field.size(),
                             mesh.cellCount())};
  }
  if (conditions.size() != mesh.faceCount()) {
    return Error{ErrorCode::InvalidArgument, "boundary conditions do not match the mesh"};
  }

  mesh_ = &mesh;
  const std::size_t cells = mesh.cellCount();

  wallDistance_ = wallDistances(mesh, conditions);

  // Freestream k and omega from the two things a user can actually state.
  //
  //   k     = (3/2) (I U)^2, since each of the three fluctuation components
  //           contributes (1/2)(I U)^2 to (1/2)<u'.u'>.
  //   omega = rho k / mu_t, inverted from mu_t = rho k / omega with mu_t set by
  //           the requested ratio.
  double referenceSpeed = 0.0;
  double referenceDensity = 0.0;
  double referenceViscosity = 0.0;
  for (std::size_t c = 0; c < cells; ++c) {
    const double speed = length(field.velocity[c]);
    if (speed > referenceSpeed) {
      referenceSpeed = speed;
    }
    referenceDensity = std::max(referenceDensity, field.density[c]);
    referenceViscosity = std::max(referenceViscosity, field.viscosity[c]);
  }
  if (!(referenceSpeed > 0.0) || !(referenceDensity > 0.0) || !(referenceViscosity > 0.0)) {
    return Error{ErrorCode::InvalidArgument,
                 "the starting field has no scale to set freestream turbulence from"};
  }

  const double intensity = std::max(inflow.intensity, 1.0e-8);
  kInflow_ = 1.5 * (intensity * referenceSpeed) * (intensity * referenceSpeed);
  const double eddyViscosity = inflow.viscosityRatio * referenceViscosity;
  omegaInflow_ = std::max(referenceDensity * kInflow_ / eddyViscosity, kMinOmega);

  k_.assign(cells, kInflow_);
  omega_.assign(cells, omegaInflow_);
  eddyViscosity_.assign(cells, eddyViscosity);

  strain_.assign(cells, 0.0);
  f1_.assign(cells, 0.0);
  f2_.assign(cells, 0.0);
  crossDiffusion_.assign(cells, 0.0);
  production_.assign(cells, 0.0);
  gradK_.assign(cells, Vec2{});
  gradOmega_.assign(cells, Vec2{});

  maxYPlus_ = 0.0;
  residuals_ = TurbulenceResiduals{};
  return Status::ok();
}

void KOmegaSST::computeStrain(const TurbulenceContext& context) {
  const std::vector<Vec2>& gradU = *context.gradU;
  const std::vector<Vec2>& gradV = *context.gradV;

  for (std::size_t c = 0; c < strain_.size(); ++c) {
    // S_ij = (1/2)(du_i/dx_j + du_j/dx_i); in two dimensions that is three
    // distinct components.
    const double s11 = gradU[c].x;
    const double s22 = gradV[c].y;
    const double s12 = 0.5 * (gradU[c].y + gradV[c].x);
    // The invariant the model uses is sqrt(2 S_ij S_ij), summing over both
    // off-diagonal positions - hence the factor of two on s12.
    strain_[c] = std::sqrt(2.0 * (s11 * s11 + s22 * s22 + 2.0 * s12 * s12));
  }
}

void KOmegaSST::computeBlending(const TurbulenceContext& context) {
  const mesh::Mesh& mesh = *mesh_;
  const flow::FlowField& field = *context.field;
  const std::vector<double>& mu = *context.molecularViscosity;
  const std::vector<double>& massFlux = *context.massFlux;

  // Gradients of k and omega, needed for the cross-diffusion term.
  std::vector<double> faceValues;
  interpolate(mesh, *context.conditions, k_, 0.0, kInflow_, massFlux, faceValues);
  if (auto grad = greenGaussGradient(mesh, faceValues); grad) {
    gradK_ = std::move(grad).value();
  }
  interpolate(mesh, *context.conditions, omega_, omega_.empty() ? 0.0 : omega_[0], omegaInflow_,
              massFlux, faceValues);
  // The wall value of omega is per-face, so a single scalar cannot express it;
  // using the owner's value gives a zero-gradient face, which is the right
  // treatment for the *gradient* even though the transport equation fixes omega
  // in the first cell outright.
  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    if (mesh.faces()[f].neighbour < 0 && mesh::oppositeCell(mesh, f) < 0 &&
        (*context.conditions)[f].kind == flow::BoundaryKind::NoSlipWall) {
      faceValues[f] = omega_[static_cast<std::size_t>(mesh.faces()[f].owner)];
    }
  }
  if (auto grad = greenGaussGradient(mesh, faceValues); grad) {
    gradOmega_ = std::move(grad).value();
  }

  const SSTConstants& k = constants_;
  for (std::size_t c = 0; c < f1_.size(); ++c) {
    const double d = std::max(wallDistance_[c], kTiny);
    const double rho = field.density[c];
    const double nu = mu[c] / std::max(rho, kTiny);
    const double omega = std::max(omega_[c], kMinOmega);
    const double energy = std::max(k_[c], 0.0);

    // CD_komega: the cross-diffusion term, floored so it can be divided by.
    const double raw = 2.0 * rho * k.sigmaOmega2 * dot(gradK_[c], gradOmega_[c]) / omega;
    crossDiffusion_[c] = std::max(raw, 1.0e-10);

    // arg1 is the smallest of three competing length-scale ratios, each of
    // which is large inside the boundary layer and small outside it:
    //   sqrt(k)/(beta* omega d)  turbulent length over wall distance
    //   500 nu/(d^2 omega)       the viscous sublayer term, which keeps F1 at 1
    //                            right down to the wall
    //   4 rho sigma_w2 k/(CD d^2) the term that switches k-epsilon back on where
    //                            cross-diffusion matters
    const double term1 = std::sqrt(std::max(energy, 0.0)) / (k.betaStar * omega * d);
    const double term2 = 500.0 * nu / (d * d * omega);
    const double term3 = 4.0 * rho * k.sigmaOmega2 * energy / (crossDiffusion_[c] * d * d);
    const double arg1 = std::min(std::max(term1, term2), term3);
    f1_[c] = std::tanh(std::clamp(arg1 * arg1 * arg1 * arg1, 0.0, 50.0));

    // arg2 drops the cross-diffusion term: F2 is only needed to mark out "am I
    // in a boundary layer", for the shear-stress limiter.
    const double arg2 = std::max(2.0 * term1, term2);
    f2_[c] = std::tanh(std::clamp(arg2 * arg2, 0.0, 50.0));
  }
}

void KOmegaSST::updateEddyViscosity(const TurbulenceContext& context) {
  const flow::FlowField& field = *context.field;
  const SSTConstants& c = constants_;

  for (std::size_t i = 0; i < eddyViscosity_.size(); ++i) {
    const double energy = std::max(k_[i], 0.0);
    const double omega = std::max(omega_[i], kMinOmega);
    // The SST limiter. Without the second term in the denominator this is just
    // mu_t = rho k / omega; with it, mu_t is capped wherever the strain rate is
    // high enough that the shear stress would otherwise exceed a1 k - which is
    // what stops the model resisting separation under an adverse gradient.
    const double denominator = std::max(c.a1 * omega, strain_[i] * f2_[i]);
    eddyViscosity_[i] = field.density[i] * c.a1 * energy / std::max(denominator, kTiny);
  }
}

void KOmegaSST::applyWallConditions(const TurbulenceContext& context) {
  const mesh::Mesh& mesh = *mesh_;
  const flow::FlowField& field = *context.field;
  const std::vector<double>& mu = *context.molecularViscosity;

  maxYPlus_ = 0.0;

  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    if ((*context.conditions)[f].kind != flow::BoundaryKind::NoSlipWall) {
      continue;
    }
    const auto owner = static_cast<std::size_t>(mesh.faces()[f].owner);
    const double d = std::max(wallDistance_[owner], kTiny);
    const double rho = std::max(field.density[owner], kTiny);
    const double nu = mu[owner] / rho;

    // Menter's low-Reynolds wall value. Integrating the omega equation through
    // the viscous sublayer gives an analytic near-wall solution, and this is it
    // evaluated at the first cell centre. It is a *fixed value*, not a
    // gradient: omega is singular at a wall, so there is nothing to extrapolate
    // and the first cell is set outright.
    omega_[owner] = 60.0 * nu / (constants_.beta1 * d * d);
    // No turbulent energy at a wall: the fluctuations vanish with the velocity.
    k_[owner] = 0.0;

    // y+ = u_tau d / nu, with u_tau from the wall shear the mean field implies.
    // Reported rather than used: the wall treatment above is only valid while
    // the first cell is inside the viscous sublayer, and a model being used
    // outside its range should be able to say so.
    const Vec2 normal = mesh.faceNormals()[f] * -1.0;
    const Vec2& velocity = field.velocity[owner];
    const Vec2 parallel = velocity - normal * dot(velocity, normal);
    const double tangential = length(parallel);
    const double wallShear = mu[owner] * tangential / d;
    const double frictionVelocity = std::sqrt(wallShear / rho);
    maxYPlus_ = std::max(maxYPlus_, frictionVelocity * d / std::max(nu, kTiny));
  }
}

void KOmegaSST::solveTransport(const TurbulenceContext& context) {
  const mesh::Mesh& mesh = *mesh_;
  const flow::FlowField& field = *context.field;
  const std::vector<double>& mu = *context.molecularViscosity;
  const std::vector<double>& massFlux = *context.massFlux;
  const SSTConstants& c = constants_;

  const std::size_t cells = mesh.cellCount();
  LinearSystem system(cells, mesh.faceCount());

  // Production of turbulent energy, P_k = mu_t S^2, limited so a stagnation
  // point - where the strain rate is large but the physical production is not -
  // cannot make k run away. This is the single most important limiter for an
  // aerofoil: without it the leading edge manufactures turbulence that then
  // convects over the whole section.
  for (std::size_t i = 0; i < cells; ++i) {
    const double raw = eddyViscosity_[i] * strain_[i] * strain_[i];
    const double ceiling =
        c.productionLimit * c.betaStar * field.density[i] * std::max(k_[i], 0.0) *
        std::max(omega_[i], kMinOmega);
    production_[i] = std::min(raw, ceiling);
  }

  const auto blend = [&](double near, double far, std::size_t i) {
    return f1_[i] * near + (1.0 - f1_[i]) * far;
  };

  // Assemble and solve one scalar transport equation. The two equations differ
  // only in their diffusion coefficient and their source terms, so they share
  // everything else - including the convecting fluxes, which are the same ones
  // the momentum equations used.
  const auto solve = [&](std::vector<double>& phi, double wallValue, double inflowValue,
                         const std::vector<double>& sigma,
                         const std::vector<double>& sourceExplicit,
                         const std::vector<double>& sourceImplicit, double floor) {
    system.clear();

    for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
      const mesh::Face& face = mesh.faces()[f];
      const auto owner = static_cast<std::size_t>(face.owner);
      const int other = (face.neighbour >= 0) ? face.neighbour : mesh::oppositeCell(mesh, f);

      const Vec2 area = mesh.faceNormals()[f] * mesh.faceAreas()[f];
      const double flux = massFlux[f];

      if (other >= 0) {
        const auto neighbour = static_cast<std::size_t>(other);
        const double w = ownerWeight(mesh, f);
        const double diffusivity =
            w * (mu[owner] + sigma[owner] * eddyViscosity_[owner]) +
            (1.0 - w) * (mu[neighbour] + sigma[neighbour] * eddyViscosity_[neighbour]);

        const Vec2 delta = mesh.cellCentroids()[neighbour] - mesh.cellCentroids()[owner];
        const double areaDotDelta = dot(area, delta);
        const double geometric =
            (std::abs(areaDotDelta) > kTiny) ? dot(area, area) / areaDotDelta : 0.0;
        const double diffusion = diffusivity * std::abs(geometric);

        // First-order upwind. The turbulence equations are far stiffer than the
        // momentum ones and a bounded scheme matters more here than accuracy:
        // an overshoot that drives k negative is not a small error, it is a
        // quantity that has stopped meaning anything.
        const double outward = std::max(flux, 0.0);
        const double inward = std::max(-flux, 0.0);

        system.diagonal[owner] += outward + diffusion;
        system.upper[f] += -inward - diffusion;
        if (face.neighbour >= 0) {
          system.diagonal[neighbour] += inward + diffusion;
          system.lower[f] += -outward - diffusion;
        }
        continue;
      }

      // Boundary face.
      const double diffusivity = mu[owner] + sigma[owner] * eddyViscosity_[owner];
      const Vec2 delta = mesh.faceCentres()[f] - mesh.cellCentroids()[owner];
      const double areaDotDelta = dot(area, delta);
      const double geometric =
          (std::abs(areaDotDelta) > kTiny) ? dot(area, area) / areaDotDelta : 0.0;
      const double diffusion = diffusivity * std::abs(geometric);

      const flow::BoundaryKind kind = (*context.conditions)[f].kind;
      const bool imposed = kind == flow::BoundaryKind::NoSlipWall ||
                           kind == flow::BoundaryKind::Inlet ||
                           (kind == flow::BoundaryKind::FarField && flux < 0.0);

      if (imposed) {
        const double value =
            (kind == flow::BoundaryKind::NoSlipWall) ? wallValue : inflowValue;
        system.diagonal[owner] += diffusion + std::max(flux, 0.0);
        system.source[owner] += (diffusion - std::min(flux, 0.0)) * value;
      } else {
        // Zero gradient: the face takes the cell's value, so convection out
        // costs the diagonal and diffusion contributes nothing.
        system.diagonal[owner] += std::max(flux, 0.0);
      }
    }

    // Sources. Production is explicit and positive; destruction is implicit,
    // which is what keeps the quantity positive: a larger phi makes its own
    // sink larger, and the diagonal grows with it.
    for (std::size_t i = 0; i < cells; ++i) {
      const double volume = mesh.cellAreas()[i];
      system.source[i] += sourceExplicit[i] * volume;
      system.diagonal[i] += sourceImplicit[i] * volume;
    }

    // Under-relax implicitly, the same way the momentum equations do.
    const double relaxation = std::clamp(context.relaxation, 0.05, 1.0);
    for (std::size_t i = 0; i < cells; ++i) {
      if (std::abs(system.diagonal[i]) > kTiny) {
        const double relaxed = system.diagonal[i] / relaxation;
        system.source[i] += (relaxed - system.diagonal[i]) * phi[i];
        system.diagonal[i] = relaxed;
      }
    }

    const double residual = system.residualL1(mesh, phi) /
                            std::max(system.diagonalL1() * std::max(inflowValue, kTiny), kTiny);

    gaussSeidel(mesh, system, phi, 3);
    for (std::size_t i = 0; i < cells; ++i) {
      phi[i] = std::max(phi[i], floor);
    }
    return residual;
  };

  // --- k ---
  std::vector<double> sigmaK(cells, 0.0);
  std::vector<double> kProduction(cells, 0.0);
  std::vector<double> kDestruction(cells, 0.0);
  for (std::size_t i = 0; i < cells; ++i) {
    sigmaK[i] = blend(c.sigmaK1, c.sigmaK2, i);
    kProduction[i] = production_[i];
    // beta* rho omega k, written as (beta* rho omega) * k so it lands on the
    // diagonal.
    kDestruction[i] = c.betaStar * field.density[i] * std::max(omega_[i], kMinOmega);
  }
  residuals_.first = solve(k_, 0.0, kInflow_, sigmaK, kProduction, kDestruction, kMinEnergy);

  // --- omega ---
  std::vector<double> sigmaOmega(cells, 0.0);
  std::vector<double> omegaProduction(cells, 0.0);
  std::vector<double> omegaDestruction(cells, 0.0);
  for (std::size_t i = 0; i < cells; ++i) {
    sigmaOmega[i] = blend(c.sigmaOmega1, c.sigmaOmega2, i);
    const double gamma = blend(c.gamma1(), c.gamma2(), i);
    const double beta = blend(c.beta1, c.beta2, i);
    const double rho = field.density[i];

    // Production, written in the form that avoids dividing by mu_t: the
    // textbook (gamma rho / mu_t) P_k becomes gamma S^2 rho once P_k = mu_t S^2
    // is substituted, which is both cheaper and safe where mu_t vanishes.
    omegaProduction[i] = gamma * rho * strain_[i] * strain_[i];
    // Plus the cross-diffusion term, active only away from the wall - this is
    // the single term by which k-epsilon differs from k-omega.
    omegaProduction[i] += (1.0 - f1_[i]) * crossDiffusion_[i];
    omegaDestruction[i] = beta * rho * std::max(omega_[i], kMinOmega);
  }
  residuals_.second =
      solve(omega_, omegaInflow_, omegaInflow_, sigmaOmega, omegaProduction, omegaDestruction,
            kMinOmega);
}

void KOmegaSST::update(const TurbulenceContext& context) {
  if (mesh_ == nullptr || context.mesh != mesh_ || context.field == nullptr ||
      context.conditions == nullptr || context.massFlux == nullptr ||
      context.gradU == nullptr || context.gradV == nullptr ||
      context.molecularViscosity == nullptr) {
    return;
  }
  if (context.conditions->size() != mesh_->faceCount()) {
    return;
  }

  computeStrain(context);
  computeBlending(context);
  solveTransport(context);
  applyWallConditions(context);
  updateEddyViscosity(context);
}

}  // namespace cfd::solver
