#include "cfd/mesh/BoxGrid.hpp"

#include <cmath>
#include <format>

#include "cfd/mesh/Distribution.hpp"

namespace cfd::mesh {

Result<Mesh> generateBox(const BoxOptions& options) {
  if (options.cellsX < 1 || options.cellsY < 1) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("a box needs at least one cell per direction, got {}x{}",
                             options.cellsX, options.cellsY)};
  }
  if (!std::isfinite(options.length) || options.length <= 0.0 ||
      !std::isfinite(options.height) || options.height <= 0.0) {
    return Error{ErrorCode::InvalidArgument, "box extents must be positive and finite"};
  }
  if (options.grading != Grading::Uniform &&
      (!(options.firstCellHeight > 0.0) || options.firstCellHeight >= 0.5)) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("firstCellHeight must lie in (0, 0.5), got {}",
                             options.firstCellHeight)};
  }

  const std::vector<double> xFraction = uniformDistribution(options.cellsX);
  std::vector<double> yFraction;
  switch (options.grading) {
    case Grading::Uniform:
      yFraction = uniformDistribution(options.cellsY);
      break;
    case Grading::TowardLower:
      yFraction = geometricDistribution(options.firstCellHeight, 1.0, options.cellsY);
      break;
    case Grading::TowardBoth:
      yFraction =
          symmetricGeometricDistribution(options.firstCellHeight, 1.0, options.cellsY);
      break;
  }

  const int ni = options.cellsX + 1;
  const int nj = options.cellsY + 1;

  StructuredMeshSpec spec;
  spec.nodesI = ni;
  spec.nodesJ = nj;
  spec.nodes.reserve(static_cast<std::size_t>(ni) * static_cast<std::size_t>(nj));
  for (int j = 0; j < nj; ++j) {
    const double y = options.originY + options.height * yFraction[static_cast<std::size_t>(j)];
    for (int i = 0; i < ni; ++i) {
      const double x =
          options.originX + options.length * xFraction[static_cast<std::size_t>(i)];
      spec.nodes.push_back(Vec2{x, y});
    }
  }

  // j = 0 is the lower edge, j = max the upper, i = 0 the left, i = max the
  // right - which follows from building the nodes with i along x and j along y.
  spec.jMinBoundary.assign(static_cast<std::size_t>(ni - 1), options.lower);
  spec.jMaxBoundary = options.upper;
  spec.iMinBoundary = options.left;
  spec.iMaxBoundary = options.right;

  return buildStructured(std::move(spec));
}

}  // namespace cfd::mesh
