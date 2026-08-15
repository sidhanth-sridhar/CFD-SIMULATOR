// BoundaryConditions.hpp - what the fluid is told to do at the edges of the domain.
//
// The Navier-Stokes equations describe the interior. They have infinitely many
// solutions until you say what happens at the boundary, and *which* solution
// you get - attached or separated, lifting or not - is decided there. This is
// the part of a CFD setup that is easiest to get quietly wrong.
//
// A finite-volume solver never needs a boundary condition in the abstract. It
// needs, for every boundary face, a velocity and a pressure to build a flux
// from. So "applying boundary conditions" here means exactly that: turn the
// interior state plus the stated conditions into a value on every face. The
// same routine also fills in the interior faces by interpolation, because the
// flux assembly in Phase 4 wants one array covering all of them.

#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

#include "cfd/core/Error.hpp"
#include "cfd/core/Vec2.hpp"
#include "cfd/flow/FlowField.hpp"
#include "cfd/flow/Freestream.hpp"
#include "cfd/mesh/Mesh.hpp"

namespace cfd::flow {

/// The kinds of condition this solver understands.
///
/// Each fixes some quantities and lets the others come from the interior. You
/// cannot fix everything: the equations already determine the rest, and
/// over-specifying makes the problem insoluble.
enum class BoundaryKind {
  /// Not a boundary. Values are interpolated from the cells on both sides.
  /// The wake cut uses this: its two sides coincide in space and fluid crosses
  /// freely, so it is an interior connection wearing a boundary's clothes.
  Internal,

  /// Velocity imposed, pressure taken from inside.
  ///
  /// Fluid is being pushed in at a known rate. The pressure needed to do that
  /// is whatever the interior says it is, so it is extrapolated rather than
  /// set - fixing both would over-determine the problem.
  Inlet,

  /// Pressure imposed, velocity taken from inside.
  ///
  /// The mirror image: fluid leaves at whatever rate the interior produces,
  /// against a known back pressure. Imposing a velocity here instead is the
  /// classic way to make an incompressible solve fail outright, because
  /// nothing then guarantees that as much leaves as entered.
  Outlet,

  /// Undisturbed stream, behaving as an inlet or an outlet per face.
  ///
  /// The outer boundary of an external flow is not all inflow. Whether a given
  /// face lets fluid in or out depends on the sign of u.n there, so each face
  /// is classified and treated accordingly. Applying a plain inlet condition
  /// to the whole outer boundary would force fluid in through faces the flow
  /// is trying to leave by.
  FarField,

  /// Solid surface: the fluid sticks to it, u = 0.
  ///
  /// Both components vanish - no flow *through* the surface, which is
  /// geometry, and no flow *along* it, which is viscosity. That second half is
  /// the no-slip condition, and it is the entire origin of the boundary layer,
  /// of skin friction, and of stall. Pressure is extrapolated: across a thin
  /// boundary layer the wall-normal pressure gradient is negligible.
  NoSlipWall,
};

[[nodiscard]] std::string_view toString(BoundaryKind kind) noexcept;

/// Which condition each mesh patch carries.
///
/// The mesh tags faces geometrically - wall, far field, outlet, wake cut - and
/// this maps those tags onto physics. Keeping the two separate means the same
/// grid can be run with different conditions without regenerating it.
struct BoundaryConditions {
  BoundaryKind wall{BoundaryKind::NoSlipWall};
  BoundaryKind farField{BoundaryKind::FarField};
  BoundaryKind outlet{BoundaryKind::Outlet};
  // The wake cut is always Internal; it is not a physical boundary and
  // offering to change it would only invite a wrong answer.

  [[nodiscard]] BoundaryKind kindFor(mesh::BoundaryType type) const noexcept;

  /// Reject combinations that cannot produce a well-posed problem.
  [[nodiscard]] Status validate() const;
};

/// Velocity and pressure on every face of the mesh.
///
/// Interior and wake-cut faces are interpolated from the cells they separate;
/// every other face is given by its boundary condition. This is the direct
/// input to a flux assembly.
struct FaceState {
  std::vector<Vec2> velocity;
  std::vector<double> pressure;
  /// Condition applied at each face; Internal for interior and wake-cut faces.
  std::vector<BoundaryKind> kind;
  /// 1 where the stream enters the domain through this face, 0 otherwise.
  /// Only meaningful on far-field faces; 0 elsewhere.
  std::vector<char> inflow;

  [[nodiscard]] std::size_t size() const noexcept { return velocity.size(); }
  [[nodiscard]] bool isConsistent() const noexcept;
};

/// Apply the boundary conditions and interpolate the interior.
///
/// Fails if the field does not match the mesh.
[[nodiscard]] Result<FaceState> evaluateFaces(const mesh::Mesh& mesh, const FlowField& field,
                                              const BoundaryConditions& conditions,
                                              const FreestreamConditions& freestream);

/// Net volume flux out of each cell divided by its area, in 1/s.
///
///     div(u)|_cell  =  (1 / V) * sum_faces ( u_f . n_f ) A_f
///
/// For an incompressible flow this must be zero everywhere: whatever volume
/// enters a cell has to leave it. It is therefore the sharpest single check on
/// a field, and on the mesh underneath it - a uniform velocity gives exactly
/// zero on any valid mesh, because the outward area vectors of a closed cell
/// sum to zero.
[[nodiscard]] Result<std::vector<double>> divergence(const mesh::Mesh& mesh,
                                                     const FaceState& faces);

/// Continuity residual from a divergence field.
///
/// Reported as the root-mean-square net volume flux per cell, so it has the
/// units of the imbalance itself (m^2/s in two dimensions) rather than being
/// scaled by an arbitrary reference. The momentum entries stay zero until
/// there is a momentum equation to leave a residual behind.
[[nodiscard]] ResidualSet continuityResidual(const mesh::Mesh& mesh,
                                             const std::vector<double>& divergence);

/// Largest absolute divergence over all cells, in 1/s.
[[nodiscard]] double maxAbsDivergence(const std::vector<double>& divergence);

}  // namespace cfd::flow
