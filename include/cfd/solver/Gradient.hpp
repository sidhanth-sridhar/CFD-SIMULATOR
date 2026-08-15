// Gradient.hpp - reconstructing a gradient from cell averages.
//
// A finite-volume method stores one value per cell and knows nothing about how
// the field varies inside it. Several terms need a gradient anyway: the
// pressure force on a cell, the non-orthogonal part of the diffusion flux, and
// the Rhie-Chow interpolation that keeps pressure and velocity coupled.
//
// The Green-Gauss reconstruction comes straight from the divergence theorem
// applied to the field itself:
//
//     integral_V (grad phi) dV  =  integral_S (phi n) dS
//
// so, dividing by the cell volume,
//
//     (grad phi)_P  =  (1 / V_P) * sum_faces ( phi_f * S_f )
//
// It is exact for a linear field on any mesh whose face values are exact, and
// first-order otherwise. That is enough for the uses above, all of which are
// either corrections or are themselves only first-order accurate.

#pragma once

#include <vector>

#include "cfd/core/Error.hpp"
#include "cfd/core/Vec2.hpp"
#include "cfd/mesh/Mesh.hpp"

namespace cfd::solver {

/// Green-Gauss gradient of a cell-centred scalar.
///
/// Face values are passed in rather than interpolated here, so that boundary
/// faces carry whatever their condition imposes. Getting that wrong is a
/// common source of a gradient that is quietly wrong in the first cell off
/// every wall - which is exactly where the answer matters.
[[nodiscard]] Result<std::vector<Vec2>> greenGaussGradient(
    const mesh::Mesh& mesh, const std::vector<double>& faceValues);

/// Linear interpolation weight for the owner of a face, by distance.
///
/// Weighting by distance rather than averaging matters wherever neighbouring
/// cells differ in size, which in a graded boundary-layer mesh is everywhere.
[[nodiscard]] double ownerWeight(const mesh::Mesh& mesh, std::size_t face) noexcept;

}  // namespace cfd::solver
