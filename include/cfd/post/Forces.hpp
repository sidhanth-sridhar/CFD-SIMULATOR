// Forces.hpp - lift, drag and pitching moment, integrated from the surface.
//
// What a force actually is
// ------------------------
// The fluid touches the section only at its surface, so whatever force it
// exerts must be the sum of what it does at every point of that surface. Two
// things act there:
//
//   * Pressure, pushing perpendicular to the skin, always inwards.
//   * Wall shear, dragging along the skin in the direction the near-wall fluid
//     is moving.
//
// Add them up around the contour and that is the entire aerodynamic force.
// Nothing else is involved, and in particular nothing about circulation, thin
// aerofoil theory or lift curves enters into it - those are models *of* this
// integral, not inputs to it.
//
//     F = integral over the surface of ( -p n + tau_w t ) ds
//
// with n the unit normal pointing out of the body into the fluid, so -p n is
// the inward push of pressure, and t the unit tangent along the local flow
// direction, so tau_w t is the drag on the skin.
//
// The moment follows from the same elements, weighted by how far they act from
// a reference point:
//
//     M = integral of ( r - r_ref ) x ( -p n + tau_w t ) ds
//
// Why the freestream pressure is subtracted
// -----------------------------------------
// A closed contour has integral( n ds ) = 0 exactly, so adding a constant to
// the pressure everywhere changes nothing about the force. Mathematically the
// choice is free. Numerically it is not: at Re = 500 the reference pressure can
// be far larger than its variation over the section, and integrating the raw
// value asks for a small answer as the difference of large numbers. Using the
// gauge pressure p - p_inf keeps every term the size of the answer.
//
// Lift and drag are freestream-relative
// -------------------------------------
// The section never moves in this solver; incidence is applied by turning the
// oncoming stream. Lift and drag are defined relative to that stream - drag
// along it, lift across it - so the body-axis force is rotated by the angle of
// attack to obtain them. That single rotation is the entire "arbitrary angle of
// attack" story: the mesh, the geometry and the integral are unchanged.

#pragma once

#include "cfd/core/Error.hpp"
#include "cfd/core/Vec2.hpp"
#include "cfd/flow/Freestream.hpp"
#include "cfd/post/SurfaceData.hpp"

namespace cfd::post {

/// A force split by what produced it, in body axes, newtons per metre of span.
struct ForceSplit {
  Vec2 pressure{};
  Vec2 friction{};

  [[nodiscard]] Vec2 total() const noexcept { return pressure + friction; }
};

/// The moment about the reference point, split the same way, newtons.
///
/// Signed counter-clockwise, the mathematical convention. The aerodynamic
/// convention is the opposite, and `pitchingMoment` below carries that flip.
struct MomentSplit {
  double pressure{0.0};
  double friction{0.0};

  [[nodiscard]] double total() const noexcept { return pressure + friction; }
};

/// Everything that comes out of integrating the surface solution.
struct AerodynamicForces {
  /// Body axes: x downstream along the chord, y up. Per metre of span.
  ForceSplit force;
  MomentSplit moment;

  /// Wind axes, per metre of span. Drag is along the freestream, lift across
  /// it, both obtained by rotating the body-axis force by the incidence.
  double lift{0.0};
  double drag{0.0};
  double pressureDrag{0.0};   ///< Form drag: the pressure part of the drag.
  double frictionDrag{0.0};   ///< Skin-friction drag.
  double pressureLift{0.0};
  double frictionLift{0.0};

  /// Pitching moment about the reference point, nose-up positive.
  double pitchingMoment{0.0};

  double liftCoefficient{0.0};
  double dragCoefficient{0.0};
  double momentCoefficient{0.0};
  double pressureDragCoefficient{0.0};
  double frictionDragCoefficient{0.0};

  /// What the coefficients were formed with, so a reader never has to guess.
  double chord{1.0};
  double dynamicPressure{0.0};
  double angleOfAttackDeg{0.0};
  Vec2 momentReference{};
  /// Stations the integral ran over. Zero means nothing was integrated.
  std::size_t stations{0};

  /// Lift-to-drag ratio, the usual measure of aerodynamic efficiency.
  ///
  /// Undefined when there is no drag, which for a viscous solution only
  /// happens if nothing has been solved yet - so the caller is told rather
  /// than handed an infinity.
  [[nodiscard]] bool hasLiftToDrag() const noexcept;
  [[nodiscard]] double liftToDrag() const noexcept;
};

/// The conventional moment reference: the quarter-chord point.
///
/// Chosen because for a thin section in attached flow the aerodynamic centre -
/// the point about which the moment barely changes with incidence - sits very
/// close to it, which makes the moment about it an almost fixed property of the
/// section rather than a number that moves with every degree of alpha.
[[nodiscard]] Vec2 quarterChord(double chord) noexcept;

/// Integrate the surface distribution into forces and coefficients.
///
/// Fails only when the inputs cannot produce a meaningful answer: an empty
/// distribution, or a freestream with no dynamic pressure to normalise by.
[[nodiscard]] Result<AerodynamicForces> integrateForces(
    const SurfaceDistribution& surface, const flow::FreestreamConditions& freestream,
    Vec2 momentReference);

/// Integrate about the quarter chord of the distribution's own chord.
[[nodiscard]] Result<AerodynamicForces> integrateForces(
    const SurfaceDistribution& surface, const flow::FreestreamConditions& freestream);

}  // namespace cfd::post
