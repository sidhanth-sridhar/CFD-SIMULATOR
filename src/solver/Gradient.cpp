#include "cfd/solver/Gradient.hpp"

#include <format>

namespace cfd::solver {

double ownerWeight(const mesh::Mesh& mesh, std::size_t face) noexcept {
  const mesh::Face& f = mesh.faces()[face];
  if (f.neighbour < 0) {
    return 1.0;
  }
  const Vec2& centre = mesh.faceCentres()[face];
  const double toOwner = distance(centre, mesh.cellCentroids()[static_cast<std::size_t>(f.owner)]);
  const double toNeighbour =
      distance(centre, mesh.cellCentroids()[static_cast<std::size_t>(f.neighbour)]);
  const double total = toOwner + toNeighbour;
  return (total > 0.0) ? toNeighbour / total : 0.5;
}

Result<std::vector<Vec2>> greenGaussGradient(const mesh::Mesh& mesh,
                                             const std::vector<double>& faceValues) {
  if (faceValues.size() != mesh.faceCount()) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("expected {} face values, got {}", mesh.faceCount(),
                             faceValues.size())};
  }

  std::vector<Vec2> gradient(mesh.cellCount(), Vec2{0.0, 0.0});

  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    const mesh::Face& face = mesh.faces()[f];
    // Area vector, pointing out of the owner.
    const Vec2 area = mesh.faceNormals()[f] * mesh.faceAreas()[f];
    const Vec2 contribution = area * faceValues[f];

    gradient[static_cast<std::size_t>(face.owner)] += contribution;
    if (face.neighbour >= 0) {
      // The same face points the other way as seen from the neighbour.
      gradient[static_cast<std::size_t>(face.neighbour)] -= contribution;
    }
  }

  for (std::size_t c = 0; c < mesh.cellCount(); ++c) {
    const double volume = mesh.cellAreas()[c];
    if (volume > 0.0) {
      gradient[c] *= 1.0 / volume;
    }
  }
  return gradient;
}

}  // namespace cfd::solver
