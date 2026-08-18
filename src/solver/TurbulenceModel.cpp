#include "cfd/solver/TurbulenceModel.hpp"

#include <cmath>
#include <format>
#include <limits>

namespace cfd::solver {

Status TurbulenceInflow::validate() const {
  if (!std::isfinite(intensity) || intensity < 0.0) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("turbulence intensity must be finite and not negative, got {}",
                             intensity)};
  }
  if (!std::isfinite(viscosityRatio) || viscosityRatio <= 0.0) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("the eddy viscosity ratio must be positive, got {}",
                             viscosityRatio)};
  }
  return Status::ok();
}

std::vector<double> wallDistances(const mesh::Mesh& mesh,
                                  const flow::FaceConditions& conditions) {
  std::vector<double> distance(mesh.cellCount(), 0.0);

  // Wall face centres, gathered once.
  std::vector<Vec2> walls;
  walls.reserve(64);
  for (std::size_t f = 0; f < mesh.faceCount() && f < conditions.size(); ++f) {
    if (conditions[f].kind == flow::BoundaryKind::NoSlipWall) {
      walls.push_back(mesh.faceCentres()[f]);
    }
  }

  if (walls.empty()) {
    // No wall is a legitimate configuration - a periodic channel, a free shear
    // layer - and the right answer there is "infinitely far", which for a
    // wall-damping function means no damping. A large finite number says that
    // without risking an infinity propagating into an arithmetic expression.
    const double far = 1.0e6 * std::sqrt(std::abs(mesh.totalArea()) + 1.0);
    std::fill(distance.begin(), distance.end(), far);
    return distance;
  }

  for (std::size_t c = 0; c < mesh.cellCount(); ++c) {
    const Vec2& centroid = mesh.cellCentroids()[c];
    double best = std::numeric_limits<double>::max();
    for (const Vec2& wall : walls) {
      const double dx = centroid.x - wall.x;
      const double dy = centroid.y - wall.y;
      // Compared squared, rooted once: this is the hot loop of the whole
      // routine and a square root per pair is most of its cost.
      const double squared = dx * dx + dy * dy;
      best = std::min(best, squared);
    }
    distance[c] = std::sqrt(best);
  }
  return distance;
}

Status LaminarModel::initialise(const mesh::Mesh& mesh,
                                const flow::FaceConditions& /*conditions*/,
                                const flow::FlowField& /*field*/,
                                const TurbulenceInflow& /*inflow*/) {
  eddyViscosity_.assign(mesh.cellCount(), 0.0);
  return Status::ok();
}

void LaminarModel::update(const TurbulenceContext& /*context*/) {
  // Nothing to advance. "The flow is laminar" is a closure with no state.
}

}  // namespace cfd::solver
