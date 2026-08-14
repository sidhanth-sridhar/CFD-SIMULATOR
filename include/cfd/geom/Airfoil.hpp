// Airfoil.hpp - discretised airfoil geometry.
//
// How a section is assembled
// --------------------------
// Given the camber line y_c(x) and the half-thickness y_t(x), the two surfaces
// are obtained by stepping off the thickness *perpendicular to the camber
// line*, not vertically:
//
//   theta = atan( dy_c/dx )
//
//   upper:   x_u = x - y_t sin(theta)     y_u = y_c + y_t cos(theta)
//   lower:   x_l = x + y_t sin(theta)     y_l = y_c - y_t cos(theta)
//
// The sin(theta) terms shift the surface points *along* the chord as well as
// across it, which is why the upper and lower surfaces of a cambered section
// do not share the same x stations. Offsetting vertically instead is a common
// shortcut; it thins the section wherever the camber line is steep, which is
// exactly the region near the leading edge that matters most.
//
// For a symmetric section theta is zero everywhere, the shift vanishes, and
// the two surfaces reduce to +/- y_t at the same stations.

#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include "cfd/core/Error.hpp"
#include "cfd/core/Vec2.hpp"
#include "cfd/geom/Naca4.hpp"

namespace cfd::geom {

/// Discretisation and sizing controls.
struct AirfoilOptions {
  /// Points per surface, including both the leading and trailing edge points.
  /// 121 resolves the nose well while staying cheap to redraw.
  int pointsPerSurface{121};

  /// Chord length in metres. The NACA equations are defined on a unit chord;
  /// every coordinate is scaled by this at the end.
  double chord{1.0};

  TrailingEdge trailingEdge{TrailingEdge::Open};
};

/// A generated section, in world coordinates: leading edge at the origin,
/// chord running along +x, y up.
///
/// The reported properties are *measured from the generated points*, not
/// copied from the designation. That is deliberate - it makes them a check on
/// the geometry rather than a restatement of the input, and it is what the
/// tests assert against.
class Airfoil {
 public:
  /// Upper surface, ordered from the leading edge to the trailing edge.
  [[nodiscard]] const std::vector<Vec2>& upper() const noexcept { return upper_; }
  /// Lower surface, ordered from the leading edge to the trailing edge.
  [[nodiscard]] const std::vector<Vec2>& lower() const noexcept { return lower_; }
  /// Mean camber line, leading edge to trailing edge. Flat for a symmetric
  /// section.
  [[nodiscard]] const std::vector<Vec2>& camberLine() const noexcept { return camber_; }

  /// The closed outline, in the conventional airfoil ordering: start at the
  /// trailing edge, forward over the upper surface to the leading edge, then
  /// aft along the lower surface back to the trailing edge. The first point is
  /// repeated at the end, so front() == back() and the sequence can be drawn
  /// or integrated as a closed loop directly.
  ///
  /// This traversal is counter-clockwise, so the shoelace area is positive.
  [[nodiscard]] const std::vector<Vec2>& contour() const noexcept { return contour_; }

  /// Normalised chordwise stations x/c used to build the surfaces, from 0 to 1.
  /// upper()[i] and lower()[i] both derive from stations()[i].
  [[nodiscard]] const std::vector<double>& stations() const noexcept { return stations_; }

  [[nodiscard]] const Naca4Digit& designation() const noexcept { return designation_; }
  [[nodiscard]] double chord() const noexcept { return chord_; }
  [[nodiscard]] TrailingEdge trailingEdgeStyle() const noexcept { return trailingEdge_; }

  [[nodiscard]] Vec2 leadingEdge() const noexcept { return leadingEdge_; }
  /// Midpoint of the two trailing edge points; they coincide for a closed
  /// trailing edge.
  [[nodiscard]] Vec2 trailingEdge() const noexcept { return trailingEdge_point_; }

  // --- measured properties, in metres or fractions of chord ---

  /// Greatest perpendicular distance between the surfaces, in metres.
  [[nodiscard]] double maxThickness() const noexcept { return maxThickness_; }
  /// Where that maximum occurs, as a fraction of chord.
  [[nodiscard]] double maxThicknessPosition() const noexcept { return maxThicknessPosition_; }
  /// Greatest displacement of the camber line from the chord, in metres.
  [[nodiscard]] double maxCamber() const noexcept { return maxCamber_; }
  /// Where that maximum occurs, as a fraction of chord.
  [[nodiscard]] double maxCamberPosition() const noexcept { return maxCamberPosition_; }
  /// Gap between the upper and lower trailing edge points, in metres. Zero for
  /// TrailingEdge::Closed.
  [[nodiscard]] double trailingEdgeGap() const noexcept { return trailingEdgeGap_; }

  /// Enclosed area from the shoelace formula over the contour, in m^2.
  /// Positive because the contour is counter-clockwise.
  [[nodiscard]] double area() const noexcept { return area_; }
  /// Total contour length, in metres.
  [[nodiscard]] double perimeter() const noexcept { return perimeter_; }

  /// Axis-aligned bounding box as (min, max).
  [[nodiscard]] std::pair<Vec2, Vec2> bounds() const noexcept { return {boundsMin_, boundsMax_}; }

  [[nodiscard]] std::size_t pointsPerSurface() const noexcept { return upper_.size(); }

 private:
  friend Result<Airfoil> generate(const Naca4Digit&, const AirfoilOptions&);
  Airfoil() = default;

  Naca4Digit designation_{0, 0, 12};
  double chord_{1.0};
  TrailingEdge trailingEdge_{TrailingEdge::Open};

  std::vector<double> stations_;
  std::vector<Vec2> upper_;
  std::vector<Vec2> lower_;
  std::vector<Vec2> camber_;
  std::vector<Vec2> contour_;

  Vec2 leadingEdge_{};
  Vec2 trailingEdge_point_{};
  Vec2 boundsMin_{};
  Vec2 boundsMax_{};

  double maxThickness_{0.0};
  double maxThicknessPosition_{0.0};
  double maxCamber_{0.0};
  double maxCamberPosition_{0.0};
  double trailingEdgeGap_{0.0};
  double area_{0.0};
  double perimeter_{0.0};
};

/// Build a section from a validated designation.
/// Fails if the discretisation options are unusable.
[[nodiscard]] Result<Airfoil> generate(const Naca4Digit& designation,
                                       const AirfoilOptions& options = {});

/// Parse a designation and generate in one step, e.g. makeNaca4Digit("NACA 2412").
[[nodiscard]] Result<Airfoil> makeNaca4Digit(std::string_view designation,
                                             const AirfoilOptions& options = {});

}  // namespace cfd::geom
