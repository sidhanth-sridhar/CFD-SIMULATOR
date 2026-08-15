// BoxGrid.hpp - a rectangular Cartesian mesh.
//
// Not for aerofoils. This exists so the solver can be checked against problems
// whose answers are known in closed form before it is pointed at a shape whose
// answer is not.
//
// A solver that has only ever been run on the real geometry cannot be
// verified: there is nothing to compare against, and a plausible-looking
// picture is not evidence. On a rectangle there are exact solutions - a
// uniform stream, plane Poiseuille flow, the Blasius boundary layer - which
// turn "does it look right" into a number with an error bar.

#pragma once

#include "cfd/core/Error.hpp"
#include "cfd/mesh/Mesh.hpp"

namespace cfd::mesh {

/// Where the nodes bunch up across the channel.
enum class Grading {
  Uniform,
  /// Fine at y = min. For a flat plate, where the wall is the bottom.
  TowardLower,
  /// Fine at both y extremes. For a channel with a wall on each side.
  TowardBoth,
};

struct BoxOptions {
  double originX{0.0};
  double originY{0.0};
  double length{1.0};  ///< extent in x
  double height{1.0};  ///< extent in y

  int cellsX{40};
  int cellsY{20};

  Grading grading{Grading::Uniform};
  /// Height of the first cell off the graded wall, as a fraction of `height`.
  /// Ignored when grading is Uniform.
  double firstCellHeight{0.01};

  // Which condition each side carries. The defaults describe a channel: flow
  // enters on the left, leaves on the right, walls above and below.
  BoundaryType left{BoundaryType::Farfield};
  BoundaryType right{BoundaryType::Outlet};
  BoundaryType lower{BoundaryType::Wall};
  BoundaryType upper{BoundaryType::Wall};
};

/// Build the mesh. Cells are ordered with i along x and j along y, so the
/// parametrisation is right-handed and every cell area is positive.
[[nodiscard]] Result<Mesh> generateBox(const BoxOptions& options);

}  // namespace cfd::mesh
