#include "cfd/post/Forces.hpp"

#include <cmath>
#include <numbers>

namespace cfd::post {
namespace {

/// Accumulate one station's contribution.
///
/// The midpoint rule over the wall faces themselves: each station stands at a
/// face centre and carries that face's length, so this is not an approximation
/// of the surface but the surface the solver actually imposed no-slip on. A
/// closed contour therefore closes exactly, which is what makes the uniform
/// pressure test below come out at round-off rather than merely small.
struct Accumulator {
  Vec2 pressureForce{};
  Vec2 frictionForce{};
  double pressureMoment{0.0};
  double frictionMoment{0.0};
  std::size_t stations{0};

  void add(const SurfacePoint& point, double referencePressure, Vec2 reference) {
    const double ds = point.segmentLength;
    if (!(ds > 0.0)) {
      return;
    }

    // Pressure pushes inwards, against the outward normal. The freestream
    // level is removed first: it integrates to nothing around a closed body,
    // and leaving it in would form the answer as a difference of large numbers.
    const Vec2 dPressure = point.normal * (-(point.pressure - referencePressure) * ds);
    // Shear drags the skin along with the near-wall flow, which is the
    // direction the tangent already points.
    const Vec2 dFriction = point.tangent * (point.wallShear * ds);

    pressureForce = pressureForce + dPressure;
    frictionForce = frictionForce + dFriction;

    // z-component of (r - r_ref) x dF, counter-clockwise positive.
    const Vec2 arm = point.position - reference;
    pressureMoment += arm.x * dPressure.y - arm.y * dPressure.x;
    frictionMoment += arm.x * dFriction.y - arm.y * dFriction.x;
    ++stations;
  }
};

}  // namespace

bool AerodynamicForces::hasLiftToDrag() const noexcept {
  return std::isfinite(dragCoefficient) && dragCoefficient > 0.0;
}

double AerodynamicForces::liftToDrag() const noexcept {
  return hasLiftToDrag() ? liftCoefficient / dragCoefficient : 0.0;
}

Vec2 quarterChord(double chord) noexcept { return Vec2{0.25 * chord, 0.0}; }

Result<AerodynamicForces> integrateForces(const SurfaceDistribution& surface,
                                          const flow::FreestreamConditions& freestream,
                                          Vec2 momentReference) {
  if (surface.empty()) {
    return Error{ErrorCode::InvalidArgument,
                 "cannot integrate forces from an empty surface distribution"};
  }

  const double dynamicPressure = freestream.dynamicPressure();
  if (!(dynamicPressure > 0.0)) {
    return Error{ErrorCode::InvalidArgument,
                 "the freestream has no dynamic pressure to form coefficients with"};
  }
  const double chord = (surface.chord > 0.0) ? surface.chord : 1.0;

  Accumulator sum;
  for (const std::vector<SurfacePoint>* side : {&surface.upper, &surface.lower}) {
    for (const SurfacePoint& point : *side) {
      sum.add(point, freestream.referencePressure, momentReference);
    }
  }
  if (sum.stations == 0) {
    return Error{ErrorCode::InvalidArgument,
                 "no surface station carried a usable face length"};
  }

  AerodynamicForces result;
  result.force.pressure = sum.pressureForce;
  result.force.friction = sum.frictionForce;
  result.moment.pressure = sum.pressureMoment;
  result.moment.friction = sum.frictionMoment;
  result.stations = sum.stations;
  result.chord = chord;
  result.dynamicPressure = dynamicPressure;
  result.angleOfAttackDeg = freestream.angleOfAttackDeg;
  result.momentReference = momentReference;

  // Body axes to wind axes. The section is fixed and the stream is turned, so
  // drag is the component along the stream direction (cos a, sin a) and lift
  // the component across it (-sin a, cos a).
  const double alpha = freestream.angleOfAttackRad();
  const double cosA = std::cos(alpha);
  const double sinA = std::sin(alpha);

  const auto dragOf = [&](const Vec2& f) { return f.x * cosA + f.y * sinA; };
  const auto liftOf = [&](const Vec2& f) { return f.y * cosA - f.x * sinA; };

  result.pressureDrag = dragOf(result.force.pressure);
  result.frictionDrag = dragOf(result.force.friction);
  result.pressureLift = liftOf(result.force.pressure);
  result.frictionLift = liftOf(result.force.friction);
  result.drag = result.pressureDrag + result.frictionDrag;
  result.lift = result.pressureLift + result.frictionLift;

  // The aerodynamic sign convention is nose-up positive. With x running from
  // the leading edge towards the trailing edge and y up, a nose-up rotation is
  // clockwise, so it is the negative of the mathematical z-moment.
  result.pitchingMoment = -sum.pressureMoment - sum.frictionMoment;

  // Per unit span, so the reference area is the chord itself and the moment is
  // normalised by the chord squared.
  const double forceScale = dynamicPressure * chord;
  const double momentScale = forceScale * chord;
  result.liftCoefficient = result.lift / forceScale;
  result.dragCoefficient = result.drag / forceScale;
  result.pressureDragCoefficient = result.pressureDrag / forceScale;
  result.frictionDragCoefficient = result.frictionDrag / forceScale;
  result.momentCoefficient = result.pitchingMoment / momentScale;

  return result;
}

Result<AerodynamicForces> integrateForces(const SurfaceDistribution& surface,
                                          const flow::FreestreamConditions& freestream) {
  const double chord = (surface.chord > 0.0) ? surface.chord : 1.0;
  return integrateForces(surface, freestream, quarterChord(chord));
}

}  // namespace cfd::post
