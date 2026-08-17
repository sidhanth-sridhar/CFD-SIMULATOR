// SurfaceData.hpp - what the flow does at the aerofoil surface.
//
// The interior solution is not the answer anyone wants. What a section is
// judged by lives on its surface: how the pressure is distributed, how hard
// the fluid drags on the skin, and whether the boundary layer stays attached.
// This turns the cell-centred field into those quantities.
//
// The two coefficients
// --------------------
// Both are the raw quantity divided by the dynamic pressure
// q = 1/2 * rho * U^2, which is what makes them comparable between speeds,
// scales and fluids - the same reason the Reynolds number is the input rather
// than the viscosity.
//
//   Cp = (p - p_inf) / q      pressure coefficient
//   Cf = tau_w / q            skin-friction coefficient
//
// Cp = 1 exactly at a stagnation point, where the flow is brought to rest and
// all its dynamic pressure becomes static pressure. Cp < 0 wherever the flow
// has been accelerated past freestream, which over the upper surface of a
// lifting section is most of it. That suction is where lift comes from.
//
// Wall shear and separation
// -------------------------
// At a solid wall the fluid sticks to it, so the velocity climbs from zero
// through the boundary layer. The stress that implies is
//
//     tau_w = mu * d(u_t)/dn      at the wall
//
// with u_t the velocity parallel to the surface and n the distance away from
// it. Its *sign* is the interesting part. Measured along the surface in the
// direction the flow is meant to travel - leading edge to trailing edge -
// tau_w is positive while the near-wall fluid moves downstream, and negative
// where it has reversed. The point where it crosses zero is separation.
//
// That is the whole detection criterion, and nothing about it is prescribed:
// it reads the sign of a number the solver produced. If the computed flow does
// not separate, none is reported.

#pragma once

#include <cstddef>
#include <vector>

#include "cfd/core/Error.hpp"
#include "cfd/core/Vec2.hpp"
#include "cfd/flow/FlowField.hpp"
#include "cfd/flow/Freestream.hpp"
#include "cfd/mesh/Mesh.hpp"

namespace cfd::post {

/// One station on the surface, at a wall face centre.
struct SurfacePoint {
  Vec2 position{};
  /// Distance along the surface from the leading edge, in metres.
  double arcLength{0.0};
  /// Chordwise station x/c, for plotting against the conventional axis.
  double chordFraction{0.0};

  /// Unit tangent, pointing the way the boundary layer develops - away from
  /// the stagnation point, towards the trailing edge on that side.
  ///
  /// Not simply "leading edge to trailing edge". At incidence the flow divides
  /// somewhere on the lower surface, and the stations between that point and
  /// the nose have fluid running *forwards* to get round it. Measured from the
  /// geometric leading edge those look like reversed flow; measured from the
  /// stagnation point, which is where the boundary layer actually starts, they
  /// are plainly attached.
  Vec2 tangent{};
  /// Unit normal, pointing away from the wall into the fluid.
  Vec2 normal{};

  /// Length of the wall face this station stands for, in metres.
  ///
  /// The element of surface the station is responsible for. Integrating forces
  /// with these rather than with differences of arc length keeps the integral
  /// on exactly the same discrete surface the solver imposed its wall
  /// condition on, so a closed contour closes to round-off.
  double segmentLength{0.0};

  double pressure{0.0};             ///< Pa, gauge
  double pressureCoefficient{0.0};  ///< Cp

  /// Wall shear stress in Pa, signed along `tangent`. Negative means the
  /// near-wall flow has reversed.
  double wallShear{0.0};
  double skinFriction{0.0};  ///< Cf

  /// Speed of the fluid in the first cell off the wall, parallel to it. The
  /// velocity *at* the wall is zero by the no-slip condition, so this is the
  /// nearest thing to a "surface velocity" that is not identically zero.
  double nearWallSpeed{0.0};

  /// True where the wall shear is negative, i.e. the flow has reversed.
  bool reversed{false};

  /// The station where the flow divides. The wall-parallel velocity is
  /// essentially zero there and its sign is decided by which side of the
  /// dividing streamline the face centre happens to fall on, so it carries no
  /// information about separation and is excluded from the search.
  bool nearStagnation{false};
};

/// Where the boundary layer detaches, as found from the wall shear.
struct SeparationPoint {
  bool found{false};
  /// Chordwise station where the wall shear first crosses zero, interpolated
  /// between the two stations either side of the crossing.
  double chordFraction{0.0};
  double arcLength{0.0};
  Vec2 position{};
};

/// Surface distributions for both sides of the section.
///
/// Both run from the leading edge to the trailing edge, so they can be plotted
/// against the same axis and compared directly.
struct SurfaceDistribution {
  std::vector<SurfacePoint> upper;
  std::vector<SurfacePoint> lower;

  SeparationPoint upperSeparation;
  SeparationPoint lowerSeparation;

  /// Where the flow divides, taken as the point of highest surface pressure.
  /// Moves aft along the lower surface as incidence increases.
  Vec2 stagnationPosition{};
  double stagnationChordFraction{0.0};

  /// Range of Cp and Cf over both surfaces, for scaling a plot.
  double minPressureCoefficient{0.0};
  double maxPressureCoefficient{0.0};
  double minSkinFriction{0.0};
  double maxSkinFriction{0.0};

  /// Chord used to normalise the chordwise stations.
  double chord{1.0};

  [[nodiscard]] bool empty() const noexcept { return upper.empty() && lower.empty(); }
  [[nodiscard]] std::size_t pointCount() const noexcept { return upper.size() + lower.size(); }
  [[nodiscard]] bool hasSeparation() const noexcept {
    return upperSeparation.found || lowerSeparation.found;
  }
};

/// Extract the surface distributions from a solved field.
///
/// Requires a structured mesh, because the wall faces are walked in contour
/// order to build a continuous arc length; an unstructured mesh would need the
/// faces sorted first.
[[nodiscard]] Result<SurfaceDistribution> extractSurface(
    const mesh::Mesh& mesh, const flow::FlowField& field,
    const flow::FreestreamConditions& freestream, double chord);

}  // namespace cfd::post
