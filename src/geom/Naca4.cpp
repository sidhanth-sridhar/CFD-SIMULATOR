#include "cfd/geom/Naca4.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <numbers>

namespace cfd::geom {
namespace {

/// Coefficients of the NACA four-digit thickness polynomial.
///
/// The first multiplies sqrt(x); the rest multiply x, x^2, x^3 and x^4. Only
/// the last differs between the open and closed trailing edge forms. They sum
/// to 0.0021 in the open form - which is precisely the residual half-thickness
/// left at x = 1 - and to zero in the closed form.
constexpr double kA0 = 0.2969;
constexpr double kA1 = -0.1260;
constexpr double kA2 = -0.3516;
constexpr double kA3 = 0.2843;
constexpr double kA4Open = -0.1015;
constexpr double kA4Closed = -0.1036;

/// Highest thickness the four-digit family was ever tabulated for. Beyond this
/// the polynomial still evaluates, but the shape stops resembling an airfoil.
constexpr int kMaxReasonableThicknessPercent = 40;

}  // namespace

std::string Naca4Digit::digits() const {
  return std::format("{}{}{:02}", maxCamberPercent_, maxCamberPositionTenths_,
                     thicknessPercent_);
}

std::string Naca4Digit::name() const { return "NACA " + digits(); }

Result<Naca4Digit> parseNaca4Digit(std::string_view text) {
  // Strip whitespace anywhere and fold to upper case, so "naca 2412",
  // "NACA2412" and " 2412 " all reduce to the same thing.
  std::string cleaned;
  cleaned.reserve(text.size());
  for (const char c : text) {
    if (std::isspace(static_cast<unsigned char>(c)) == 0) {
      cleaned.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
  }

  if (cleaned.starts_with("NACA")) {
    cleaned.erase(0, 4);
  }

  if (cleaned.empty()) {
    return Error{ErrorCode::InvalidArgument, "no designation given (expected four digits, e.g. 2412)"};
  }
  if (cleaned.size() != 4) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("'{}' has {} digits; a four-digit designation is required "
                             "(e.g. 0012, 2412)",
                             cleaned, cleaned.size())};
  }
  for (const char c : cleaned) {
    if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
      return Error{ErrorCode::InvalidArgument,
                   std::format("'{}' contains a non-digit character", cleaned)};
    }
  }

  const int m = cleaned[0] - '0';
  const int p = cleaned[1] - '0';
  const int tt = (cleaned[2] - '0') * 10 + (cleaned[3] - '0');

  // A section with camber but with its maximum at the leading edge is not a
  // real designation, and the camber equations divide by p^2.
  if (m > 0 && p == 0) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("'{}' places maximum camber at the leading edge; the second "
                             "digit must be non-zero when the first is",
                             cleaned)};
  }
  // Camber of zero at a non-zero position is contradictory: with m = 0 the
  // camber line is identically zero, so p describes nothing.
  if (m == 0 && p > 0) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("'{}' has zero camber but a camber position of {}; a "
                             "symmetric section is written 00{:02}",
                             cleaned, p, tt)};
  }
  if (tt == 0) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("'{}' has zero thickness", cleaned)};
  }
  if (tt > kMaxReasonableThicknessPercent) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("'{}' is {}% thick; the four-digit family is only defined up "
                             "to {}%",
                             cleaned, tt, kMaxReasonableThicknessPercent)};
  }

  return Naca4Digit{m, p, tt};
}

double thicknessDistribution(double x, double thickness, TrailingEdge trailingEdge) noexcept {
  // Outside the chord the distribution is not defined; clamping keeps callers
  // from producing NaN through sqrt of a negative number.
  if (!(x > 0.0)) {
    return 0.0;  // also catches NaN
  }
  const double xc = std::min(x, 1.0);
  const double a4 = (trailingEdge == TrailingEdge::Closed) ? kA4Closed : kA4Open;

  const double x2 = xc * xc;
  const double x3 = x2 * xc;
  const double x4 = x3 * xc;

  return 5.0 * thickness *
         (kA0 * std::sqrt(xc) + kA1 * xc + kA2 * x2 + kA3 * x3 + a4 * x4);
}

double camberLine(double x, double maxCamber, double maxCamberPosition) noexcept {
  const double m = maxCamber;
  const double p = maxCamberPosition;
  if (m == 0.0 || p <= 0.0 || p >= 1.0) {
    return 0.0;  // symmetric section
  }

  const double xc = std::clamp(x, 0.0, 1.0);
  if (xc <= p) {
    return (m / (p * p)) * (2.0 * p * xc - xc * xc);
  }
  const double q = 1.0 - p;
  return (m / (q * q)) * ((1.0 - 2.0 * p) + 2.0 * p * xc - xc * xc);
}

double camberSlope(double x, double maxCamber, double maxCamberPosition) noexcept {
  const double m = maxCamber;
  const double p = maxCamberPosition;
  if (m == 0.0 || p <= 0.0 || p >= 1.0) {
    return 0.0;
  }

  const double xc = std::clamp(x, 0.0, 1.0);
  // Both branches share the factor (p - x); only the denominator differs.
  const double denominator = (xc <= p) ? (p * p) : ((1.0 - p) * (1.0 - p));
  return (2.0 * m / denominator) * (p - xc);
}

std::vector<double> cosineSpacing(int count) {
  if (count < 2) {
    return {};
  }

  const auto n = static_cast<std::size_t>(count);
  std::vector<double> stations(n);
  const double last = static_cast<double>(count - 1);

  for (std::size_t i = 0; i < n; ++i) {
    const double beta = std::numbers::pi * static_cast<double>(i) / last;
    stations[i] = 0.5 * (1.0 - std::cos(beta));
  }

  // cos(0) and cos(pi) are exact, but pin the endpoints anyway so the leading
  // and trailing edges land on exactly 0 and 1 regardless of rounding in the
  // multiplication above.
  stations.front() = 0.0;
  stations.back() = 1.0;
  return stations;
}

}  // namespace cfd::geom
