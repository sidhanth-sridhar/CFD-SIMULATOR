// CGrid.hpp - a structured C-grid around an airfoil section.
//
// Why a C-grid
// ------------
// The two standard structured topologies for an aerofoil are the O-grid, whose
// grid lines close around the body like tree rings, and the C-grid, whose
// lines wrap the nose like the letter C and then run downstream on both sides
// of a cut along the wake.
//
// The C-grid is chosen here for two reasons:
//
//   * The domain is deliberately asymmetric - far more room downstream than
//     upstream, because that is where the wake goes. An O-grid has a single
//     outer boundary and cannot express that.
//   * The wake is the physically interesting region for separation and stall,
//     and a C-grid puts a line of well-aligned, refined cells straight down
//     it. An O-grid's cells behind the trailing edge fan outwards and smear
//     exactly what we most want to resolve.
//
// The cost is the wake cut: a slit from the trailing edge to the outflow where
// the grid is opened up so it can be a topological rectangle. The two sides of
// the slit touch in space but are separate faces, matched through Face::partner.
//
// Layout in computational space
// -----------------------------
// The grid is a rectangular block of (i, j) nodes:
//
//   j = 0        the inner boundary, traversed as
//                outflow -> wake cut -> TRAILING EDGE -> lower surface ->
//                LEADING EDGE -> upper surface -> TRAILING EDGE ->
//                wake cut -> outflow
//   j = max      the outer boundary: two straight lines at y = +/- vertical
//                extent joined round the front by a half ellipse
//   i = 0, max   the downstream outflow plane
//
// Increasing j always moves away from the body.

#pragma once

#include <string_view>

#include "cfd/core/Error.hpp"
#include "cfd/geom/Airfoil.hpp"
#include "cfd/mesh/Mesh.hpp"

namespace cfd::mesh {

/// Named refinement levels. Everything they set is also settable by hand.
enum class MeshResolution { Coarse, Medium, Fine };

[[nodiscard]] std::string_view toString(MeshResolution resolution) noexcept;

struct CGridOptions {
  // --- domain extent, in chord lengths ---
  // The far boundary has to be far enough that pretending the flow there is
  // undisturbed freestream does not corrupt the answer near the body. An
  // aerofoil's influence decays slowly, so "far" means chord lengths, not
  // percentages, and the wake needs much more room than the nose.
  double upstreamChords{12.0};    ///< ahead of the leading edge
  double downstreamChords{25.0};  ///< behind the trailing edge
  double verticalChords{12.0};    ///< above and below the chord line

  // --- discretisation ---
  int surfacePoints{160};  ///< nodes per surface; the wall gets 2*this - 2
  int wakePoints{56};      ///< nodes along each side of the wake cut
  int normalPoints{72};    ///< nodes from the wall out to the far field

  /// Height of the first cell off the wall, as a fraction of chord. This is
  /// the single most important meshing number for a viscous calculation: the
  /// boundary layer is resolved only if several cells fit inside it.
  double firstLayerHeight{3e-4};

  /// Thickness of the wall-orthogonal zone, as a fraction of chord. Inside it
  /// grid lines leave the surface along its normal; beyond it they straighten
  /// towards the far field. See the note on folding in CGrid.cpp.
  double normalBlendLength{0.06};

  /// How many passes of smoothing are applied to the wall-normal directions.
  /// Softens the corner where the wake cut meets the trailing edge.
  int normalSmoothingPasses{6};
};

/// Preset for a named resolution. Each level roughly halves the spacing.
[[nodiscard]] CGridOptions optionsFor(MeshResolution resolution);

/// Generate a C-grid around `airfoil`.
///
/// The section is re-evaluated from its designation at `surfacePoints`, so the
/// wall follows the analytic NACA equations at the mesh's own resolution
/// rather than being interpolated from whatever the display happened to use.
///
/// Requires a closed trailing edge: the wake cut has to start from a single
/// point, and a blunt base would leave the C topology with a gap it cannot
/// represent.
[[nodiscard]] Result<Mesh> generateCGrid(const geom::Airfoil& airfoil,
                                         const CGridOptions& options = {});

}  // namespace cfd::mesh
