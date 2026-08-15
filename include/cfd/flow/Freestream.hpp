// Freestream.hpp - the operating point: what the air is doing far from the wing.
//
// Everything is SI: metres, seconds, kilograms, so velocities are m/s,
// densities kg/m^3, pressures Pa and viscosities Pa.s.
//
// Why the Reynolds number is an input rather than an output
// ---------------------------------------------------------
// A section's behaviour is not set by the speed, the size or the fluid
// separately, but by one dimensionless combination of them:
//
//     Re = rho * U * c / mu
//
// It is the ratio of inertial forces to viscous forces. Two flows with the
// same Re over the same shape are the same flow, whatever the actual speed and
// scale - which is why a model in a wind tunnel says anything about a full-size
// wing. It also decides the character of the answer: at Re ~ 10^3 the flow is
// orderly and viscosity dominates, at the 10^6 typical of aerofoil work the
// boundary layer is thin and usually turbulent, and separation and stall
// depend on which.
//
// So the natural way to pose the problem is "give me Re", and let the
// viscosity follow, exactly as an experimenter picks a tunnel speed to hit a
// target Re. Specifying a real air viscosity instead would fix Re at whatever
// the geometry and speed happened to produce.

#pragma once

#include "cfd/core/Error.hpp"
#include "cfd/core/Vec2.hpp"

namespace cfd::flow {

/// Conditions in the undisturbed stream, far ahead of the section.
struct FreestreamConditions {
  /// Magnitude of the oncoming velocity, m/s.
  double speed{50.0};

  /// Direction of the oncoming flow relative to the chord line, in degrees.
  /// Positive pitches the nose up, which is the sign convention that makes
  /// lift increase with incidence.
  double angleOfAttackDeg{0.0};

  /// Mass per unit volume, kg/m^3. Constant here: at the speeds this solver
  /// targets the Mach number is low enough that the flow is incompressible to
  /// well under a percent, so density carries no useful information and
  /// treating it as constant removes an entire equation.
  double density{1.225};

  /// Static pressure of the undisturbed stream, Pa. Only differences in
  /// pressure drive an incompressible flow, so this is a gauge datum and zero
  /// is the natural choice.
  double referencePressure{0.0};

  /// Reynolds number based on the chord. 1e6 is the usual ballpark for
  /// aerofoil measurements and for the reference data this project will
  /// eventually be compared against.
  double reynoldsNumber{1.0e6};

  /// Freestream velocity vector, U * (cos alpha, sin alpha).
  [[nodiscard]] Vec2 velocity() const noexcept;

  /// Dynamic pressure q = 1/2 * rho * U^2, Pa.
  ///
  /// The natural unit for aerodynamic loads: dividing a force by q and by a
  /// reference area is what turns it into the lift and drag *coefficients*
  /// that make different conditions comparable. Phase 6 will need it.
  [[nodiscard]] double dynamicPressure() const noexcept;

  /// Angle of attack in radians.
  [[nodiscard]] double angleOfAttackRad() const noexcept;

  /// Dynamic viscosity implied by the Reynolds number at this chord,
  /// mu = rho * U * c / Re, in Pa.s.
  [[nodiscard]] double dynamicViscosity(double chord) const noexcept;

  /// Kinematic viscosity nu = mu / rho, m^2/s.
  [[nodiscard]] double kinematicViscosity(double chord) const noexcept;

  /// Reject conditions that would make the equations meaningless - a
  /// non-positive speed, density or Reynolds number, or a non-finite angle.
  [[nodiscard]] Status validate() const;
};

}  // namespace cfd::flow
