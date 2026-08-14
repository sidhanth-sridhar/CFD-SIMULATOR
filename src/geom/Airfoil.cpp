#include "cfd/geom/Airfoil.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "cfd/core/Log.hpp"

namespace cfd::geom {
namespace {

constexpr std::string_view kLogCategory = "geom";

/// Below this a "surface" cannot even represent a leading edge, a mid station
/// and a trailing edge.
constexpr int kMinPointsPerSurface = 3;
/// Guards against an accidental huge allocation from a mistyped input.
constexpr int kMaxPointsPerSurface = 100000;

/// Signed area of a closed polygon by the shoelace formula:
///
///   A = 1/2 * sum_i ( x_i * y_{i+1} - x_{i+1} * y_i )
///
/// Each term is twice the signed area of the triangle formed by the origin and
/// one edge; the contributions outside the polygon cancel. The sign encodes
/// the traversal direction, positive for counter-clockwise.
double shoelaceArea(const std::vector<Vec2>& closedLoop) {
  if (closedLoop.size() < 4) {
    return 0.0;
  }
  double twiceArea = 0.0;
  // The loop already repeats its first point at the end, so stopping one short
  // covers every edge exactly once.
  for (std::size_t i = 0; i + 1 < closedLoop.size(); ++i) {
    twiceArea += cross(closedLoop[i], closedLoop[i + 1]);
  }
  return 0.5 * twiceArea;
}

double polylineLength(const std::vector<Vec2>& points) {
  double total = 0.0;
  for (std::size_t i = 0; i + 1 < points.size(); ++i) {
    total += distance(points[i], points[i + 1]);
  }
  return total;
}

}  // namespace

Result<Airfoil> generate(const Naca4Digit& designation, const AirfoilOptions& options) {
  if (options.pointsPerSurface < kMinPointsPerSurface) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("pointsPerSurface must be at least {}, got {}",
                             kMinPointsPerSurface, options.pointsPerSurface)};
  }
  if (options.pointsPerSurface > kMaxPointsPerSurface) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("pointsPerSurface must be at most {}, got {}",
                             kMaxPointsPerSurface, options.pointsPerSurface)};
  }
  if (!std::isfinite(options.chord) || options.chord <= 0.0) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("chord must be a positive finite length, got {}", options.chord)};
  }

  const double m = designation.maxCamber();
  const double p = designation.maxCamberPosition();
  const double t = designation.thickness();
  const double c = options.chord;

  Airfoil airfoil;
  airfoil.designation_ = designation;
  airfoil.chord_ = c;
  airfoil.trailingEdge_ = options.trailingEdge;
  airfoil.stations_ = cosineSpacing(options.pointsPerSurface);

  const std::size_t n = airfoil.stations_.size();
  airfoil.upper_.reserve(n);
  airfoil.lower_.reserve(n);
  airfoil.camber_.reserve(n);

  for (const double x : airfoil.stations_) {
    const double yc = camberLine(x, m, p);
    const double yt = thicknessDistribution(x, t, options.trailingEdge);

    // Rotate the thickness offset into the direction normal to the camber
    // line. For a symmetric section the slope is zero, so theta is zero, sin
    // vanishes and cos is one - the offsets become purely vertical.
    const double theta = std::atan(camberSlope(x, m, p));
    const double sinTheta = std::sin(theta);
    const double cosTheta = std::cos(theta);

    airfoil.camber_.push_back(Vec2{x * c, yc * c});
    airfoil.upper_.push_back(Vec2{(x - yt * sinTheta) * c, (yc + yt * cosTheta) * c});
    airfoil.lower_.push_back(Vec2{(x + yt * sinTheta) * c, (yc - yt * cosTheta) * c});
  }

  // Both surfaces start from the same leading edge point, where the thickness
  // is zero. Assign it explicitly so the two agree bit for bit rather than to
  // within rounding.
  airfoil.upper_.front() = airfoil.camber_.front();
  airfoil.lower_.front() = airfoil.camber_.front();
  if (options.trailingEdge == TrailingEdge::Closed) {
    // Likewise at the trailing edge, where the closed-form polynomial is zero
    // by construction.
    airfoil.upper_.back() = airfoil.camber_.back();
    airfoil.lower_.back() = airfoil.camber_.back();
  }

  // --- closed contour, in the conventional ordering ---
  // Trailing edge, forward over the upper surface, round the nose, then aft
  // along the lower surface and back to the start.
  airfoil.contour_.reserve(2 * n);
  for (std::size_t i = n; i-- > 0;) {
    airfoil.contour_.push_back(airfoil.upper_[i]);
  }
  // Skip the lower surface's first point: it is the leading edge, already
  // added as the last point of the reversed upper surface. With a closed
  // trailing edge, skip its last point too, since that coincides with where
  // the loop started.
  const std::size_t lowerEnd = (options.trailingEdge == TrailingEdge::Closed) ? n - 1 : n;
  for (std::size_t i = 1; i < lowerEnd; ++i) {
    airfoil.contour_.push_back(airfoil.lower_[i]);
  }
  airfoil.contour_.push_back(airfoil.contour_.front());

  // --- measured properties ---
  airfoil.leadingEdge_ = airfoil.upper_.front();
  airfoil.trailingEdge_point_ =
      (airfoil.upper_.back() + airfoil.lower_.back()) * 0.5;
  airfoil.trailingEdgeGap_ = distance(airfoil.upper_.back(), airfoil.lower_.back());

  // Local thickness is the distance between corresponding surface points,
  // which is a perpendicular measurement because that is how they were built.
  for (std::size_t i = 0; i < n; ++i) {
    const double localThickness = distance(airfoil.upper_[i], airfoil.lower_[i]);
    if (localThickness > airfoil.maxThickness_) {
      airfoil.maxThickness_ = localThickness;
      airfoil.maxThicknessPosition_ = airfoil.stations_[i];
    }
    const double localCamber = std::abs(airfoil.camber_[i].y);
    if (localCamber > airfoil.maxCamber_) {
      airfoil.maxCamber_ = localCamber;
      airfoil.maxCamberPosition_ = airfoil.stations_[i];
    }
  }

  airfoil.area_ = shoelaceArea(airfoil.contour_);
  airfoil.perimeter_ = polylineLength(airfoil.contour_);

  airfoil.boundsMin_ = airfoil.contour_.front();
  airfoil.boundsMax_ = airfoil.contour_.front();
  for (const Vec2& point : airfoil.contour_) {
    airfoil.boundsMin_.x = std::min(airfoil.boundsMin_.x, point.x);
    airfoil.boundsMin_.y = std::min(airfoil.boundsMin_.y, point.y);
    airfoil.boundsMax_.x = std::max(airfoil.boundsMax_.x, point.x);
    airfoil.boundsMax_.y = std::max(airfoil.boundsMax_.y, point.y);
  }

  CFD_LOG_DEBUG(kLogCategory,
                "{}: {} points/surface, chord {:.4g} m, max thickness {:.4f} c at {:.3f} c",
                designation.name(), n, c, airfoil.maxThickness_ / c,
                airfoil.maxThicknessPosition_);

  return airfoil;
}

Result<Airfoil> makeNaca4Digit(std::string_view designation, const AirfoilOptions& options) {
  const Result<Naca4Digit> parsed = parseNaca4Digit(designation);
  if (!parsed) {
    return parsed.error();
  }
  return generate(parsed.value(), options);
}

}  // namespace cfd::geom
