#include "cfd/flow/BoundaryConditions.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace cfd::flow {
namespace {

/// Linear interpolation weight for the owner of an interior face.
///
/// Weighting by distance rather than taking a plain average matters wherever
/// the two cells are different sizes - which, in a boundary-layer mesh whose
/// cells grow geometrically away from the wall, is everywhere. A face sitting
/// close to the owner's centroid should mostly take the owner's value.
double ownerWeight(const Vec2& faceCentre, const Vec2& ownerCentroid,
                   const Vec2& neighbourCentroid) noexcept {
  const double toOwner = distance(faceCentre, ownerCentroid);
  const double toNeighbour = distance(faceCentre, neighbourCentroid);
  const double total = toOwner + toNeighbour;
  if (!(total > 0.0)) {
    return 0.5;
  }
  return toNeighbour / total;
}

}  // namespace

std::string_view toString(BoundaryKind kind) noexcept {
  switch (kind) {
    case BoundaryKind::Internal:   return "Internal";
    case BoundaryKind::Inlet:      return "Inlet";
    case BoundaryKind::Outlet:     return "Outlet";
    case BoundaryKind::FarField:   return "Far field";
    case BoundaryKind::NoSlipWall: return "No-slip wall";
  }
  return "Unknown";
}

BoundaryKind BoundaryConditions::kindFor(mesh::BoundaryType type) const noexcept {
  switch (type) {
    case mesh::BoundaryType::Interior: return BoundaryKind::Internal;
    case mesh::BoundaryType::WakeCut:  return BoundaryKind::Internal;
    case mesh::BoundaryType::Wall:     return wall;
    case mesh::BoundaryType::Farfield: return farField;
    case mesh::BoundaryType::Outlet:   return outlet;
  }
  return BoundaryKind::Internal;
}

Status BoundaryConditions::validate() const {
  // An incompressible problem needs somewhere for pressure to be anchored.
  // With velocity imposed on every boundary the pressure is only determined up
  // to a constant, and the linear system for it is singular. At least one
  // patch must therefore set a pressure.
  const bool anchorsPressure = outlet == BoundaryKind::Outlet ||
                               outlet == BoundaryKind::FarField ||
                               farField == BoundaryKind::Outlet ||
                               farField == BoundaryKind::FarField;
  if (!anchorsPressure) {
    return Error{ErrorCode::InvalidArgument,
                 "no boundary sets a pressure: an incompressible problem with velocity "
                 "imposed everywhere leaves pressure undetermined up to a constant"};
  }
  if (wall == BoundaryKind::Internal) {
    return Error{ErrorCode::InvalidArgument,
                 "the airfoil surface cannot be an internal face; fluid would pass "
                 "straight through it"};
  }
  return Status::ok();
}

bool FaceState::isConsistent() const noexcept {
  const std::size_t n = velocity.size();
  return pressure.size() == n && kind.size() == n && inflow.size() == n;
}

Result<FaceState> evaluateFaces(const mesh::Mesh& mesh, const FlowField& field,
                                const BoundaryConditions& conditions,
                                const FreestreamConditions& freestream) {
  if (!field.isConsistent()) {
    return Error{ErrorCode::Internal, "flow field arrays have inconsistent lengths"};
  }
  if (field.size() != mesh.cellCount()) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("field has {} cells but the mesh has {}", field.size(),
                             mesh.cellCount())};
  }
  if (const Status valid = conditions.validate(); !valid) {
    return valid.error();
  }
  if (const Status valid = freestream.validate(); !valid) {
    return valid.error();
  }

  const Vec2 streamVelocity = freestream.velocity();

  FaceState state;
  const std::size_t faceCount = mesh.faceCount();
  state.velocity.assign(faceCount, Vec2{0.0, 0.0});
  state.pressure.assign(faceCount, 0.0);
  state.kind.assign(faceCount, BoundaryKind::Internal);
  state.inflow.assign(faceCount, char{0});

  for (std::size_t f = 0; f < faceCount; ++f) {
    const mesh::Face& face = mesh.faces()[f];
    const auto owner = static_cast<std::size_t>(face.owner);

    // Which cell sits on the far side, if any. For a wake cut that is the
    // owner of the partner face: the two coincide in space, so the pair
    // behaves exactly like one interior face.
    int otherCell = face.neighbour;
    if (face.boundary == mesh::BoundaryType::WakeCut && face.partner >= 0) {
      otherCell = mesh.faces()[static_cast<std::size_t>(face.partner)].owner;
    }

    const BoundaryKind kind = conditions.kindFor(face.boundary);
    state.kind[f] = kind;

    if (kind == BoundaryKind::Internal && otherCell >= 0) {
      const auto neighbour = static_cast<std::size_t>(otherCell);
      const double w = ownerWeight(mesh.faceCentres()[f], mesh.cellCentroids()[owner],
                                   mesh.cellCentroids()[neighbour]);
      state.velocity[f] = field.velocity[owner] * w + field.velocity[neighbour] * (1.0 - w);
      state.pressure[f] = field.pressure[owner] * w + field.pressure[neighbour] * (1.0 - w);
      continue;
    }

    switch (kind) {
      case BoundaryKind::NoSlipWall:
        // The fluid sticks to the surface. Pressure comes from inside: across
        // a thin boundary layer the wall-normal gradient is negligible.
        state.velocity[f] = Vec2{0.0, 0.0};
        state.pressure[f] = field.pressure[owner];
        break;

      case BoundaryKind::Inlet:
        state.velocity[f] = streamVelocity;
        state.pressure[f] = field.pressure[owner];
        state.inflow[f] = char{1};
        break;

      case BoundaryKind::Outlet:
        // Velocity leaves at whatever rate the interior produces; the back
        // pressure is what is actually known.
        state.velocity[f] = field.velocity[owner];
        state.pressure[f] = freestream.referencePressure;
        break;

      case BoundaryKind::FarField: {
        // Decide per face which way the stream is crossing this boundary. The
        // normal points out of the domain, so a negative dot product means
        // fluid is coming in.
        const double outward = dot(streamVelocity, mesh.faceNormals()[f]);
        const bool entering = outward < 0.0;
        state.inflow[f] = entering ? char{1} : char{0};

        if (entering) {
          state.velocity[f] = streamVelocity;
          state.pressure[f] = field.pressure[owner];
        } else {
          state.velocity[f] = field.velocity[owner];
          state.pressure[f] = freestream.referencePressure;
        }
        break;
      }

      case BoundaryKind::Internal:
        // An internal face with nothing on the far side: the wake cut lost its
        // partner. Fall back to the owner's value rather than leave a zero
        // that would silently look like a wall.
        state.velocity[f] = field.velocity[owner];
        state.pressure[f] = field.pressure[owner];
        break;
    }
  }

  return state;
}

Result<std::vector<double>> divergence(const mesh::Mesh& mesh, const FaceState& faces) {
  if (!faces.isConsistent()) {
    return Error{ErrorCode::Internal, "face state arrays have inconsistent lengths"};
  }
  if (faces.size() != mesh.faceCount()) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("face state has {} faces but the mesh has {}", faces.size(),
                             mesh.faceCount())};
  }

  std::vector<double> netFlux(mesh.cellCount(), 0.0);

  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    const mesh::Face& face = mesh.faces()[f];
    // The normal points out of the owner, so this flux leaves the owner and,
    // for an interior face, enters the neighbour.
    const double flux = dot(faces.velocity[f], mesh.faceNormals()[f]) * mesh.faceAreas()[f];

    netFlux[static_cast<std::size_t>(face.owner)] += flux;
    if (face.neighbour >= 0) {
      netFlux[static_cast<std::size_t>(face.neighbour)] -= flux;
    }
    // A wake cut face needs no special case: its partner carries the opposite
    // normal, so the matching flux is subtracted from the cell on the far side
    // when that face is visited.
  }

  std::vector<double> result(mesh.cellCount(), 0.0);
  for (std::size_t c = 0; c < mesh.cellCount(); ++c) {
    const double area = mesh.cellAreas()[c];
    result[c] = (area > 0.0) ? netFlux[c] / area : 0.0;
  }
  return result;
}

ResidualSet continuityResidual(const mesh::Mesh& mesh,
                               const std::vector<double>& divergenceField) {
  ResidualSet residuals;
  if (divergenceField.empty() || divergenceField.size() != mesh.cellCount()) {
    return residuals;
  }

  double sumSquares = 0.0;
  for (std::size_t c = 0; c < divergenceField.size(); ++c) {
    // Back out the net flux so the residual reflects the actual volumetric
    // imbalance, not a rate that tiny near-wall cells would exaggerate.
    const double netFlux = divergenceField[c] * mesh.cellAreas()[c];
    sumSquares += netFlux * netFlux;
  }

  residuals.continuity = std::sqrt(sumSquares / static_cast<double>(divergenceField.size()));
  // Momentum residuals stay zero: there is no momentum equation yet to leave
  // anything behind.
  return residuals;
}

double maxAbsDivergence(const std::vector<double>& divergenceField) {
  double worst = 0.0;
  for (const double value : divergenceField) {
    worst = std::max(worst, std::abs(value));
  }
  return worst;
}

}  // namespace cfd::flow
