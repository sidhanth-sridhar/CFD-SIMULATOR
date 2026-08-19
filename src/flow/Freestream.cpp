#include "cfd/flow/Freestream.hpp"

#include <cmath>
#include <format>
#include <numbers>

namespace cfd::flow {

double FreestreamConditions::angleOfAttackRad() const noexcept {
  return angleOfAttackDeg * std::numbers::pi / 180.0;
}

Vec2 FreestreamConditions::velocity() const noexcept {
  const double alpha = angleOfAttackRad();
  return Vec2{speed * std::cos(alpha), speed * std::sin(alpha)};
}

double FreestreamConditions::dynamicPressure() const noexcept {
  return 0.5 * density * speed * speed;
}

double FreestreamConditions::dynamicViscosity(double chord) const noexcept {
  // A stated viscosity wins; otherwise it follows from the Reynolds number.
  if (dynamicViscosityOverride > 0.0) {
    return dynamicViscosityOverride;
  }
  return (reynoldsNumber > 0.0) ? density * speed * chord / reynoldsNumber : 0.0;
}

double FreestreamConditions::effectiveReynolds(double chord) const noexcept {
  const double mu = dynamicViscosity(chord);
  return (mu > 0.0) ? density * speed * chord / mu : 0.0;
}

double FreestreamConditions::kinematicViscosity(double chord) const noexcept {
  if (!(density > 0.0)) {
    return 0.0;
  }
  return dynamicViscosity(chord) / density;
}

Status FreestreamConditions::validate() const {
  if (!std::isfinite(speed) || speed <= 0.0) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("freestream speed must be positive, got {}", speed)};
  }
  if (!std::isfinite(density) || density <= 0.0) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("density must be positive, got {}", density)};
  }
  if (!std::isfinite(reynoldsNumber) || reynoldsNumber <= 0.0) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("Reynolds number must be positive, got {}", reynoldsNumber)};
  }
  if (!std::isfinite(angleOfAttackDeg) || std::abs(angleOfAttackDeg) > 90.0) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("angle of attack must lie within +/-90 degrees, got {}",
                             angleOfAttackDeg)};
  }
  if (!std::isfinite(referencePressure)) {
    return Error{ErrorCode::InvalidArgument, "reference pressure must be finite"};
  }
  return Status::ok();
}

}  // namespace cfd::flow
