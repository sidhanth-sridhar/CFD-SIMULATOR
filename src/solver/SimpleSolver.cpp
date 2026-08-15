#include "cfd/solver/SimpleSolver.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "cfd/solver/Gradient.hpp"

namespace cfd::solver {
namespace {

/// Component accessor, so the momentum assembly can be written once and run
/// for x and y.
double component(const Vec2& v, int index) noexcept { return (index == 0) ? v.x : v.y; }

constexpr double kTiny = 1e-300;

}  // namespace

std::string_view toString(ConvectionScheme scheme) noexcept {
  switch (scheme) {
    case ConvectionScheme::Upwind:            return "Upwind";
    case ConvectionScheme::SecondOrderUpwind: return "Second-order upwind";
  }
  return "Unknown";
}

SimpleSolver::SimpleSolver(const mesh::Mesh& mesh) : mesh_(&mesh) {}

Result<SimpleSolver> SimpleSolver::create(const mesh::Mesh& mesh,
                                          flow::FaceConditions conditions,
                                          const SimpleSettings& settings) {
  if (mesh.cellCount() == 0) {
    return Error{ErrorCode::InvalidArgument, "cannot solve on an empty mesh"};
  }
  if (conditions.size() != mesh.faceCount()) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("expected {} face conditions, got {}", mesh.faceCount(),
                             conditions.size())};
  }
  if (!(settings.velocityRelaxation > 0.0) || settings.velocityRelaxation > 1.0 ||
      !(settings.pressureRelaxation > 0.0) || settings.pressureRelaxation > 1.0) {
    return Error{ErrorCode::InvalidArgument, "relaxation factors must lie in (0, 1]"};
  }

  SimpleSolver solver(mesh);
  solver.conditions_ = std::move(conditions);
  solver.settings_ = settings;

  const std::size_t cells = mesh.cellCount();
  const std::size_t faces = mesh.faceCount();
  solver.massFlux_.assign(faces, 0.0);
  solver.faceU_.assign(faces, 0.0);
  solver.faceV_.assign(faces, 0.0);
  solver.faceP_.assign(faces, 0.0);
  solver.gradU_.assign(cells, Vec2{});
  solver.gradV_.assign(cells, Vec2{});
  solver.gradP_.assign(cells, Vec2{});
  solver.momentum_ = LinearSystem(cells, faces);
  solver.pressure_ = LinearSystem(cells, faces);
  solver.momentumDiagonal_.assign(cells, 1.0);
  solver.netFlux_.assign(cells, 0.0);

  solver.precomputeGeometry();

  // Whether the pressure level is pinned by a boundary decides how the
  // pressure equation has to be treated: with no anchor it is singular.
  for (std::size_t f = 0; f < faces; ++f) {
    const flow::BoundaryKind kind = solver.conditions_[f].kind;
    if (kind == flow::BoundaryKind::Outlet || kind == flow::BoundaryKind::FarField) {
      solver.hasPressureReference_ = true;
      break;
    }
  }

  return solver;
}

void SimpleSolver::precomputeGeometry() {
  const mesh::Mesh& mesh = *mesh_;
  geometry_.assign(mesh.faceCount(), FaceGeometry{});

  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    const mesh::Face& face = mesh.faces()[f];
    FaceGeometry& g = geometry_[f];

    g.area = mesh.faceNormals()[f] * mesh.faceAreas()[f];

    const Vec2& ownerCentroid = mesh.cellCentroids()[static_cast<std::size_t>(face.owner)];
    if (face.neighbour >= 0) {
      g.delta = mesh.cellCentroids()[static_cast<std::size_t>(face.neighbour)] - ownerCentroid;
      g.ownerWeight = ownerWeight(mesh, f);
    } else {
      // For a boundary face the "neighbour" is the face itself.
      g.delta = mesh.faceCentres()[f] - ownerCentroid;
      g.ownerWeight = 1.0;
    }

    // Over-relaxed decomposition S = E + T, with E parallel to d. Splitting
    // this way keeps the implicit part of the diffusion as large as possible,
    // which is what keeps the matrix diagonally dominant on a skewed mesh; the
    // leftover T is handled explicitly as a deferred correction.
    const double areaDotDelta = dot(g.area, g.delta);
    const double areaSquared = dot(g.area, g.area);
    if (std::abs(areaDotDelta) > kTiny) {
      g.diffusion = areaSquared / areaDotDelta;
      g.tangential = g.area - g.delta * g.diffusion;
    } else {
      // Degenerate: the face is edge-on to the line joining the centroids.
      g.diffusion = 0.0;
      g.tangential = Vec2{};
    }
  }
}

Status SimpleSolver::initialise(const flow::FlowField& initial) {
  if (!initial.isConsistent() || initial.size() != mesh_->cellCount()) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("initial field has {} cells but the mesh has {}",
                             initial.size(), mesh_->cellCount())};
  }
  field_ = initial;

  interpolateFaceValues();

  // Seed the face fluxes from the starting field. In a collocated method the
  // face flux is a variable in its own right - it, not the cell velocity, is
  // what continuity is imposed on.
  const mesh::Mesh& mesh = *mesh_;
  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    const auto owner = static_cast<std::size_t>(mesh.faces()[f].owner);
    const double w = geometry_[f].ownerWeight;
    double density = field_.density[owner];
    if (mesh.faces()[f].neighbour >= 0) {
      const auto neighbour = static_cast<std::size_t>(mesh.faces()[f].neighbour);
      density = w * field_.density[owner] + (1.0 - w) * field_.density[neighbour];
    }
    massFlux_[f] = density * (faceU_[f] * geometry_[f].area.x + faceV_[f] * geometry_[f].area.y);
  }

  // Reference flow rate for normalising the continuity residual: everything
  // entering the domain. Without it the residual would depend on the size of
  // the case rather than on how converged it is.
  referenceMassFlow_ = 0.0;
  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    if (mesh.faces()[f].isInterior() ||
        conditions_[f].kind == flow::BoundaryKind::Internal) {
      continue;
    }
    referenceMassFlow_ += std::max(-massFlux_[f], 0.0);
  }
  if (!(referenceMassFlow_ > 0.0)) {
    referenceMassFlow_ = 1.0;
  }

  return Status::ok();
}

bool SimpleSolver::imposesVelocity(std::size_t face) const {
  switch (conditions_[face].kind) {
    case flow::BoundaryKind::NoSlipWall:
    case flow::BoundaryKind::Inlet:
      return true;
    case flow::BoundaryKind::FarField:
      // The outer boundary of an external flow takes fluid in at the front and
      // lets it out behind, so the condition follows the current flux.
      return massFlux_[face] < 0.0;
    case flow::BoundaryKind::Outlet:
    case flow::BoundaryKind::Internal:
      return false;
  }
  return false;
}

Vec2 SimpleSolver::boundaryVelocity(std::size_t face) const {
  return conditions_[face].velocity;
}

void SimpleSolver::interpolateFaceValues() {
  const mesh::Mesh& mesh = *mesh_;

  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    const mesh::Face& face = mesh.faces()[f];
    const auto owner = static_cast<std::size_t>(face.owner);

    // A wake cut is geometrically a boundary but physically interior: its
    // partner supplies the cell on the far side.
    int other = face.neighbour;
    if (other < 0 && face.boundary == mesh::BoundaryType::WakeCut && face.partner >= 0) {
      other = mesh.faces()[static_cast<std::size_t>(face.partner)].owner;
    }

    if (other >= 0) {
      const auto neighbour = static_cast<std::size_t>(other);
      const double w = geometry_[f].ownerWeight;
      faceU_[f] = w * field_.velocity[owner].x + (1.0 - w) * field_.velocity[neighbour].x;
      faceV_[f] = w * field_.velocity[owner].y + (1.0 - w) * field_.velocity[neighbour].y;
      faceP_[f] = w * field_.pressure[owner] + (1.0 - w) * field_.pressure[neighbour];
      continue;
    }

    if (imposesVelocity(f)) {
      const Vec2 imposed = boundaryVelocity(f);
      faceU_[f] = imposed.x;
      faceV_[f] = imposed.y;
      // Pressure is extrapolated wherever velocity is imposed.
      faceP_[f] = field_.pressure[owner];
    } else {
      // Pressure imposed, velocity extrapolated.
      faceU_[f] = field_.velocity[owner].x;
      faceV_[f] = field_.velocity[owner].y;
      faceP_[f] = conditions_[f].pressure;
    }
  }
}

void SimpleSolver::computeGradients() {
  const mesh::Mesh& mesh = *mesh_;
  if (auto g = greenGaussGradient(mesh, faceU_); g) {
    gradU_ = std::move(g).value();
  }
  if (auto g = greenGaussGradient(mesh, faceV_); g) {
    gradV_ = std::move(g).value();
  }
  if (auto g = greenGaussGradient(mesh, faceP_); g) {
    gradP_ = std::move(g).value();
  }
}

void SimpleSolver::assembleMomentum(int index) {
  const mesh::Mesh& mesh = *mesh_;
  momentum_.clear();

  const std::vector<Vec2>& gradient = (index == 0) ? gradU_ : gradV_;

  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    const mesh::Face& face = mesh.faces()[f];
    const FaceGeometry& g = geometry_[f];
    const auto owner = static_cast<std::size_t>(face.owner);

    int other = face.neighbour;
    if (other < 0 && face.boundary == mesh::BoundaryType::WakeCut && face.partner >= 0) {
      other = mesh.faces()[static_cast<std::size_t>(face.partner)].owner;
    }

    const double flux = massFlux_[f];

    if (other >= 0 && face.neighbour >= 0) {
      const auto neighbour = static_cast<std::size_t>(other);
      const double w = g.ownerWeight;
      const double viscosity =
          w * field_.viscosity[owner] + (1.0 - w) * field_.viscosity[neighbour];
      const double diffusion = viscosity * g.diffusion;

      // Convection, first-order upwind: the face value is taken from whichever
      // side the flow is coming from.
      const double outward = std::max(flux, 0.0);
      const double inward = std::max(-flux, 0.0);

      momentum_.diagonal[owner] += outward + diffusion;
      momentum_.upper[f] += -inward - diffusion;
      momentum_.diagonal[neighbour] += inward + diffusion;
      momentum_.lower[f] += -outward - diffusion;

      // Non-orthogonal correction, evaluated from the previous iteration's
      // gradient. Explicit because making it implicit would fill the matrix
      // with entries beyond the immediate neighbours.
      const Vec2 faceGradient = gradient[owner] * w + gradient[neighbour] * (1.0 - w);
      const double correction = viscosity * dot(faceGradient, g.tangential);
      momentum_.source[owner] += correction;
      momentum_.source[neighbour] -= correction;

      if (settings_.scheme == ConvectionScheme::SecondOrderUpwind) {
        // Deferred correction towards a linear interpolation. The implicit
        // part stays upwind, so the matrix keeps its diagonal dominance while
        // the answer converges to the more accurate scheme.
        const double upwindValue =
            (flux >= 0.0) ? component(field_.velocity[owner], index)
                          : component(field_.velocity[neighbour], index);
        const double centralValue = w * component(field_.velocity[owner], index) +
                                    (1.0 - w) * component(field_.velocity[neighbour], index);
        const double deferred = flux * (centralValue - upwindValue);
        momentum_.source[owner] -= deferred;
        momentum_.source[neighbour] += deferred;
      }
      continue;
    }

    if (other >= 0) {
      // Wake cut: interior physics, but the two sides are separate faces so
      // only the owner's row is touched here. The partner face contributes the
      // matching term to the cell on the other side when it is visited.
      const auto neighbour = static_cast<std::size_t>(other);
      const double viscosity = field_.viscosity[owner];
      const double diffusion = viscosity * g.diffusion;
      const double outward = std::max(flux, 0.0);
      const double inward = std::max(-flux, 0.0);

      momentum_.diagonal[owner] += outward + diffusion;
      // The neighbour is not a face neighbour in the matrix, so its influence
      // has to be explicit.
      momentum_.source[owner] +=
          (inward + diffusion) * component(field_.velocity[neighbour], index);
      continue;
    }

    // --- boundary face ---
    const double viscosity = field_.viscosity[owner];
    const double diffusion = viscosity * g.diffusion;

    if (imposesVelocity(f)) {
      const double imposed = component(boundaryVelocity(f), index);
      // Convection of a known value is entirely explicit.
      momentum_.source[owner] -= flux * imposed;
      momentum_.diagonal[owner] += diffusion;
      momentum_.source[owner] += diffusion * imposed;
      momentum_.source[owner] += viscosity * dot(gradient[owner], g.tangential);
    } else {
      // Zero-gradient: the face value is the cell value, so convection is
      // implicit and there is no diffusive flux.
      momentum_.diagonal[owner] += flux;
    }
  }

  // Pressure gradient, integrated over the cell.
  for (std::size_t c = 0; c < mesh.cellCount(); ++c) {
    momentum_.source[c] -= component(gradP_[c], index) * mesh.cellAreas()[c];
  }
}

void SimpleSolver::applyRelaxation(int index) {
  const mesh::Mesh& mesh = *mesh_;
  const double alpha = settings_.velocityRelaxation;

  for (std::size_t c = 0; c < mesh.cellCount(); ++c) {
    const double unrelaxed = momentum_.diagonal[c];
    if (!(std::abs(unrelaxed) > 0.0)) {
      momentum_.diagonal[c] = 1.0;
      continue;
    }
    const double relaxed = unrelaxed / alpha;
    momentum_.diagonal[c] = relaxed;
    // Adding back (relaxed - unrelaxed) * phi_old leaves the converged
    // solution untouched: at convergence phi equals phi_old and the two extra
    // terms cancel exactly. Under-relaxation must not change the answer, only
    // the path to it.
    momentum_.source[c] += (relaxed - unrelaxed) * component(field_.velocity[c], index);
  }
}

void SimpleSolver::computeRhieChowFluxes() {
  const mesh::Mesh& mesh = *mesh_;

  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    const mesh::Face& face = mesh.faces()[f];
    const FaceGeometry& g = geometry_[f];
    const auto owner = static_cast<std::size_t>(face.owner);

    int other = face.neighbour;
    if (other < 0 && face.boundary == mesh::BoundaryType::WakeCut && face.partner >= 0) {
      other = mesh.faces()[static_cast<std::size_t>(face.partner)].owner;
    }

    if (other >= 0) {
      const auto neighbour = static_cast<std::size_t>(other);
      const double w = g.ownerWeight;

      const Vec2 interpolated =
          field_.velocity[owner] * w + field_.velocity[neighbour] * (1.0 - w);
      const double density =
          w * field_.density[owner] + (1.0 - w) * field_.density[neighbour];
      const double mobility = w * (mesh.cellAreas()[owner] / momentumDiagonal_[owner]) +
                              (1.0 - w) * (mesh.cellAreas()[neighbour] /
                                           momentumDiagonal_[neighbour]);
      const Vec2 smoothGradient = gradP_[owner] * w + gradP_[neighbour] * (1.0 - w);

      // Rhie-Chow: the difference between a compact pressure difference across
      // this face and the smoothly interpolated gradient. For a smooth
      // pressure field the two agree and the term vanishes; for a checkerboard
      // they disagree violently, and the term damps it out.
      const double compact =
          (field_.pressure[neighbour] - field_.pressure[owner]) * g.diffusion;
      const double smooth = dot(smoothGradient, g.area);

      massFlux_[f] = density * dot(interpolated, g.area) -
                     density * mobility * (compact - smooth);
      continue;
    }

    const double density = field_.density[owner];
    if (imposesVelocity(f)) {
      // The flux is dictated, not computed.
      massFlux_[f] = density * dot(boundaryVelocity(f), g.area);
    } else {
      const double mobility = mesh.cellAreas()[owner] / momentumDiagonal_[owner];
      const double compact = (conditions_[f].pressure - field_.pressure[owner]) * g.diffusion;
      const double smooth = dot(gradP_[owner], g.area);
      massFlux_[f] = density * dot(field_.velocity[owner], g.area) -
                     density * mobility * (compact - smooth);
    }
  }
}

void SimpleSolver::assemblePressureCorrection() {
  const mesh::Mesh& mesh = *mesh_;
  pressure_.clear();
  std::fill(netFlux_.begin(), netFlux_.end(), 0.0);

  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    const mesh::Face& face = mesh.faces()[f];
    const FaceGeometry& g = geometry_[f];
    const auto owner = static_cast<std::size_t>(face.owner);

    netFlux_[owner] += massFlux_[f];
    if (face.neighbour >= 0) {
      netFlux_[static_cast<std::size_t>(face.neighbour)] -= massFlux_[f];
    }

    // How strongly this face's flux responds to a pressure difference. It is
    // the same coefficient the flux correction uses, which is what makes the
    // corrected field satisfy continuity exactly.
    if (face.neighbour >= 0) {
      const auto neighbour = static_cast<std::size_t>(face.neighbour);
      const double w = g.ownerWeight;
      const double density =
          w * field_.density[owner] + (1.0 - w) * field_.density[neighbour];
      const double mobility = w * (mesh.cellAreas()[owner] / momentumDiagonal_[owner]) +
                              (1.0 - w) * (mesh.cellAreas()[neighbour] /
                                           momentumDiagonal_[neighbour]);
      const double coefficient = density * mobility * g.diffusion;

      pressure_.diagonal[owner] += coefficient;
      pressure_.upper[f] += -coefficient;
      pressure_.diagonal[neighbour] += coefficient;
      pressure_.lower[f] += -coefficient;
      continue;
    }

    if (face.boundary == mesh::BoundaryType::WakeCut) {
      continue;  // handled by the partner face's own row
    }
    if (imposesVelocity(f)) {
      continue;  // the flux is fixed, so no correction is possible here
    }

    // Pressure is imposed, so the correction there is zero and the whole
    // coefficient lands on the diagonal.
    const double density = field_.density[owner];
    const double mobility = mesh.cellAreas()[owner] / momentumDiagonal_[owner];
    pressure_.diagonal[owner] += density * mobility * g.diffusion;
  }

  for (std::size_t c = 0; c < mesh.cellCount(); ++c) {
    pressure_.source[c] = -netFlux_[c];
  }

  if (!hasPressureReference_) {
    // Nothing pins the pressure level, so the matrix is singular. The system
    // is still solvable as long as the right-hand side sums to zero - that is
    // just the statement that as much mass leaves the domain as enters it -
    // so remove any drift rather than pinning a cell, which would break the
    // symmetry conjugate gradient depends on.
    double mean = 0.0;
    for (const double value : pressure_.source) {
      mean += value;
    }
    mean /= static_cast<double>(pressure_.source.size());
    for (double& value : pressure_.source) {
      value -= mean;
    }
  }
}

void SimpleSolver::correct(const std::vector<double>& pressureCorrection) {
  const mesh::Mesh& mesh = *mesh_;

  // Face values of the correction, so its gradient can be reconstructed the
  // same way as any other field.
  std::vector<double> faceCorrection(mesh.faceCount(), 0.0);
  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    const mesh::Face& face = mesh.faces()[f];
    const auto owner = static_cast<std::size_t>(face.owner);

    int other = face.neighbour;
    if (other < 0 && face.boundary == mesh::BoundaryType::WakeCut && face.partner >= 0) {
      other = mesh.faces()[static_cast<std::size_t>(face.partner)].owner;
    }

    if (other >= 0) {
      const double w = geometry_[f].ownerWeight;
      faceCorrection[f] = w * pressureCorrection[owner] +
                          (1.0 - w) * pressureCorrection[static_cast<std::size_t>(other)];
    } else if (imposesVelocity(f)) {
      faceCorrection[f] = pressureCorrection[owner];  // zero gradient
    } else {
      faceCorrection[f] = 0.0;  // pressure is already imposed here
    }
  }

  auto gradientResult = greenGaussGradient(mesh, faceCorrection);
  const std::vector<Vec2> gradCorrection =
      gradientResult ? std::move(gradientResult).value()
                     : std::vector<Vec2>(mesh.cellCount(), Vec2{});

  for (std::size_t c = 0; c < mesh.cellCount(); ++c) {
    // Pressure is under-relaxed; velocity is not. The velocity correction is
    // what actually enforces continuity, so damping it would leave the field
    // divergent.
    field_.pressure[c] += settings_.pressureRelaxation * pressureCorrection[c];

    const double mobility = mesh.cellAreas()[c] / momentumDiagonal_[c];
    field_.velocity[c] -= gradCorrection[c] * mobility;
  }

  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    const mesh::Face& face = mesh.faces()[f];
    const FaceGeometry& g = geometry_[f];
    const auto owner = static_cast<std::size_t>(face.owner);

    if (face.neighbour >= 0) {
      const auto neighbour = static_cast<std::size_t>(face.neighbour);
      const double w = g.ownerWeight;
      const double density =
          w * field_.density[owner] + (1.0 - w) * field_.density[neighbour];
      const double mobility = w * (mesh.cellAreas()[owner] / momentumDiagonal_[owner]) +
                              (1.0 - w) * (mesh.cellAreas()[neighbour] /
                                           momentumDiagonal_[neighbour]);
      massFlux_[f] -= density * mobility * g.diffusion *
                      (pressureCorrection[neighbour] - pressureCorrection[owner]);
      continue;
    }
    if (face.boundary == mesh::BoundaryType::WakeCut || imposesVelocity(f)) {
      continue;
    }
    const double density = field_.density[owner];
    const double mobility = mesh.cellAreas()[owner] / momentumDiagonal_[owner];
    massFlux_[f] -= density * mobility * g.diffusion * (0.0 - pressureCorrection[owner]);
  }
}

SolverMonitor SimpleSolver::iterate() {
  const mesh::Mesh& mesh = *mesh_;
  SolverMonitor monitor;

  interpolateFaceValues();
  computeGradients();

  // --- momentum predictor ---
  std::vector<double> solution(mesh.cellCount(), 0.0);
  double momentumResidual[2] = {0.0, 0.0};

  // One velocity scale for both components, taken from the field as a whole.
  // Scaling each component by its own magnitude would make the y residual
  // meaningless in a flow that is predominantly in x - and undefined in a
  // uniform stream, where v is identically zero.
  double velocityScale = 0.0;
  for (std::size_t c = 0; c < mesh.cellCount(); ++c) {
    velocityScale = std::max(velocityScale, length(field_.velocity[c]));
  }
  if (!(velocityScale > 0.0)) {
    velocityScale = 1.0;
  }

  for (int index = 0; index < 2; ++index) {
    assembleMomentum(index);
    applyRelaxation(index);

    for (std::size_t c = 0; c < mesh.cellCount(); ++c) {
      solution[c] = component(field_.velocity[c], index);
    }

    // Measured before solving, so it describes the field we started from.
    const double residual = momentum_.residualL1(mesh, solution);
    const double scale = momentum_.diagonalL1() * velocityScale;
    momentumResidual[index] = residual / std::max(scale, kTiny);

    gaussSeidel(mesh, momentum_, solution, settings_.momentumSweeps);

    for (std::size_t c = 0; c < mesh.cellCount(); ++c) {
      if (index == 0) {
        field_.velocity[c].x = solution[c];
      } else {
        field_.velocity[c].y = solution[c];
      }
    }
    if (index == 0) {
      momentumDiagonal_ = momentum_.diagonal;
    }
  }

  // --- pressure correction ---
  computeRhieChowFluxes();

  std::vector<double> correction(mesh.cellCount(), 0.0);
  for (int pass = 0; pass < std::max(1, settings_.nonOrthogonalCorrectors); ++pass) {
    assemblePressureCorrection();

    // The continuity residual belongs to the field before any correction.
    if (pass == 0) {
      double imbalance = 0.0;
      for (const double value : netFlux_) {
        imbalance += std::abs(value);
      }
      monitor.massImbalance = imbalance;
      monitor.residuals.continuity = imbalance / referenceMassFlow_;
    }

    std::fill(correction.begin(), correction.end(), 0.0);
    const SolveReport report = conjugateGradient(mesh, pressure_, correction,
                                                 settings_.pressureTolerance,
                                                 settings_.pressureIterations);
    monitor.pressureIterations += report.iterations;

    if (!hasPressureReference_) {
      // Remove the arbitrary constant so the level does not wander.
      double mean = 0.0;
      for (const double value : correction) {
        mean += value;
      }
      mean /= static_cast<double>(correction.size());
      for (double& value : correction) {
        value -= mean;
      }
    }

    correct(correction);
  }

  monitor.residuals.momentumX = momentumResidual[0];
  monitor.residuals.momentumY = momentumResidual[1];

  const std::vector<double> divergenceField = divergence();
  for (const double value : divergenceField) {
    monitor.maxDivergence = std::max(monitor.maxDivergence, std::abs(value));
  }

  return monitor;
}

std::vector<double> SimpleSolver::divergence() const {
  const mesh::Mesh& mesh = *mesh_;
  std::vector<double> net(mesh.cellCount(), 0.0);

  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    const mesh::Face& face = mesh.faces()[f];
    const auto owner = static_cast<std::size_t>(face.owner);
    const double density = std::max(field_.density[owner], kTiny);
    const double volumeFlux = massFlux_[f] / density;

    net[owner] += volumeFlux;
    if (face.neighbour >= 0) {
      net[static_cast<std::size_t>(face.neighbour)] -= volumeFlux;
    }
  }

  for (std::size_t c = 0; c < mesh.cellCount(); ++c) {
    const double area = mesh.cellAreas()[c];
    net[c] = (area > 0.0) ? net[c] / area : 0.0;
  }
  return net;
}

}  // namespace cfd::solver
