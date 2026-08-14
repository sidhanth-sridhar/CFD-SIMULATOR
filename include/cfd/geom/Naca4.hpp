// Naca4.hpp - the NACA four-digit airfoil family.
//
// What the digits mean
// --------------------
// A four-digit designation MPXX encodes three shape parameters, all expressed
// as fractions of the chord (the straight line from leading to trailing edge,
// and the natural length unit for a section):
//
//   M   maximum camber, in percent of chord           -> m = M / 100
//   P   chordwise position of that maximum, in tenths -> p = P / 10
//   XX  maximum thickness, in percent of chord        -> t = XX / 100
//
// So NACA 2412 is 2% camber with its maximum at 40% chord, and 12% thick.
// NACA 0012 has no camber at all: it is symmetric and 12% thick.
//
// The shape is built from two independent parts:
//
//   * the mean camber line, the curve running midway between the upper and
//     lower surfaces. Its deviation from the chord is what makes a section
//     asymmetric, and therefore what lets it generate lift at zero angle of
//     attack.
//   * a thickness distribution, laid on symmetrically either side of that
//     camber line - measured perpendicular to it, not vertically.
//
// Splitting camber from thickness this way is the defining idea of the NACA
// series. It means camber (which mostly sets the zero-lift angle) and
// thickness (which mostly sets the drag and stall behaviour) can be varied
// independently.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "cfd/core/Error.hpp"

namespace cfd::geom {

/// How the surfaces meet at x = c.
enum class TrailingEdge {
  /// The published NACA polynomial, which leaves a small blunt trailing edge
  /// of about 0.21% of the thickness (0.25% chord for a 12% section). This is
  /// what the standard equations produce and what reference ordinate tables
  /// list.
  Open,
  /// The last polynomial coefficient adjusted from -0.1015 to -0.1036 so the
  /// half-thickness reaches exactly zero at x = c. Physically no real wing has
  /// a knife edge, but a closed trailing edge avoids having to mesh a blunt
  /// base later, so it is the more convenient input to a solver.
  Closed,
};

/// A validated four-digit designation. Construct through parseNaca4Digit;
/// the fields are the raw digits, the accessors the physical fractions.
class Naca4Digit {
 public:
  /// Only parseNaca4Digit and generation code should build these directly.
  constexpr Naca4Digit(int maxCamberPercent, int maxCamberPositionTenths,
                       int thicknessPercent) noexcept
      : maxCamberPercent_(maxCamberPercent),
        maxCamberPositionTenths_(maxCamberPositionTenths),
        thicknessPercent_(thicknessPercent) {}

  [[nodiscard]] constexpr int maxCamberPercent() const noexcept { return maxCamberPercent_; }
  [[nodiscard]] constexpr int maxCamberPositionTenths() const noexcept {
    return maxCamberPositionTenths_;
  }
  [[nodiscard]] constexpr int thicknessPercent() const noexcept { return thicknessPercent_; }

  /// m: maximum camber as a fraction of chord.
  [[nodiscard]] constexpr double maxCamber() const noexcept {
    return static_cast<double>(maxCamberPercent_) / 100.0;
  }
  /// p: chordwise position of maximum camber as a fraction of chord.
  [[nodiscard]] constexpr double maxCamberPosition() const noexcept {
    return static_cast<double>(maxCamberPositionTenths_) / 10.0;
  }
  /// t: maximum thickness as a fraction of chord.
  [[nodiscard]] constexpr double thickness() const noexcept {
    return static_cast<double>(thicknessPercent_) / 100.0;
  }

  /// True when the camber line is identically zero, so the upper and lower
  /// surfaces are exact mirror images.
  [[nodiscard]] constexpr bool isSymmetric() const noexcept {
    return maxCamberPercent_ == 0 || maxCamberPositionTenths_ == 0;
  }

  /// The four digits, e.g. "2412".
  [[nodiscard]] std::string digits() const;
  /// The full name, e.g. "NACA 2412".
  [[nodiscard]] std::string name() const;

  friend constexpr bool operator==(const Naca4Digit&, const Naca4Digit&) = default;

 private:
  int maxCamberPercent_;
  int maxCamberPositionTenths_;
  int thicknessPercent_;
};

/// Parse a designation, with or without the "NACA" prefix and with any amount
/// of surrounding or internal whitespace: "NACA 2412", "naca2412" and "2412"
/// are all accepted.
///
/// Rejects anything that would not describe a real section, including a
/// non-zero camber with its position at the leading edge (which makes the
/// camber equations divide by zero) and zero thickness.
[[nodiscard]] Result<Naca4Digit> parseNaca4Digit(std::string_view text);

// ---------------------------------------------------------------------------
// The defining equations, exposed so they can be tested directly
// ---------------------------------------------------------------------------

/// Half-thickness at chordwise station x, for a unit chord.
///
///   y_t(x) = 5t ( 0.2969*sqrt(x) - 0.1260*x - 0.3516*x^2
///                 + 0.2843*x^3 - 0.1015*x^4 )
///
/// where t is the thickness fraction and x runs from 0 at the leading edge to
/// 1 at the trailing edge. The result is the distance from the camber line to
/// one surface, so the full local thickness is twice this.
///
/// The leading sqrt(x) term is what gives the airfoil its rounded nose: its
/// derivative is infinite at x = 0, so the surface meets the leading edge
/// vertically rather than in a point. The remaining polynomial terms were
/// fitted to make the maximum thickness fall at x = 0.3 and the section close
/// smoothly towards the trailing edge.
[[nodiscard]] double thicknessDistribution(double x, double thickness,
                                           TrailingEdge trailingEdge = TrailingEdge::Open) noexcept;

/// Mean camber line height at station x, for a unit chord.
///
/// Two parabolic arcs meeting at the point of maximum camber, chosen so the
/// line is continuous and has continuous slope there:
///
///   0 <= x <= p :  y_c = (m/p^2)     ( 2 p x - x^2 )
///   p <= x <= 1 :  y_c = (m/(1-p)^2) ( (1 - 2p) + 2 p x - x^2 )
///
/// Both branches give y_c = m at x = p, which is the definition of m.
/// Returns zero for a symmetric section.
[[nodiscard]] double camberLine(double x, double maxCamber, double maxCamberPosition) noexcept;

/// Slope dy_c/dx of the camber line at station x.
///
///   0 <= x <= p :  dy_c/dx = (2m/p^2)     (p - x)
///   p <= x <= 1 :  dy_c/dx = (2m/(1-p)^2) (p - x)
///
/// Both vanish at x = p, as they must at a maximum. The slope is needed
/// because thickness is applied perpendicular to the camber line, so the
/// surface offset has to be rotated by atan(dy_c/dx).
[[nodiscard]] double camberSlope(double x, double maxCamber, double maxCamberPosition) noexcept;

/// Chordwise stations clustered towards both ends:
///
///   x_i = (1 - cos(beta_i)) / 2,   beta_i = i*pi/(n-1),   i = 0..n-1
///
/// Uniform spacing wastes points in the middle of the section, where the
/// surface is nearly straight, and starves the leading edge, where curvature
/// is highest and the sqrt(x) nose changes fastest. Sampling uniformly in the
/// angle beta rather than in x concentrates points at both the leading and
/// trailing edges, which is why every airfoil code uses it.
///
/// The returned values run from exactly 0 to exactly 1 and are strictly
/// increasing. Requires n >= 2.
[[nodiscard]] std::vector<double> cosineSpacing(int count);

}  // namespace cfd::geom
