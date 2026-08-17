// Tests for the aerodynamic force integration.
//
// Two kinds of check, for two kinds of claim.
//
// The exact ones impose a field whose force is known in closed form - a
// uniform pressure exerts no net force on a closed body, a pure shear exerts no
// pressure force - and demand the integral reproduce it to round-off. Those
// test the integration itself, with no solver involved and nothing to argue
// about.
//
// The solved ones run the real thing and check the properties that make a
// force calculation believable: a symmetric section at zero incidence lifts
// nothing, incidence lifts in the direction it is applied, and the answer
// settles as the solve converges rather than wandering.

#include "cfd/post/Forces.hpp"

#include <gtest/gtest.h>

#include <cmath>

#include "cfd/flow/BoundaryConditions.hpp"
#include "cfd/geom/Airfoil.hpp"
#include "cfd/mesh/CGrid.hpp"
#include "cfd/post/SurfaceData.hpp"
#include "cfd/solver/SimpleSolver.hpp"

namespace {

using cfd::Vec2;
using cfd::flow::FlowField;
using cfd::flow::FreestreamConditions;
using cfd::mesh::Mesh;
using cfd::post::AerodynamicForces;
using cfd::post::extractSurface;
using cfd::post::integrateForces;
using cfd::post::quarterChord;
using cfd::post::SurfaceDistribution;

/// A coarse C-grid around NACA 0012, shared by the exact tests.
const Mesh& sharedMesh() {
  static const Mesh mesh = [] {
    auto section = cfd::geom::makeNaca4Digit(
        "0012", {.chord = 1.0, .trailingEdge = cfd::geom::TrailingEdge::Closed});
    EXPECT_TRUE(section);
    auto grid = cfd::mesh::generateCGrid(
        section.value(), cfd::mesh::optionsFor(cfd::mesh::MeshResolution::Coarse));
    EXPECT_TRUE(grid) << (grid.hasError() ? grid.error().format() : "");
    return std::move(grid).value();
  }();
  return mesh;
}

FreestreamConditions stream(double alpha = 0.0, double referencePressure = 0.0) {
  FreestreamConditions conditions;
  conditions.speed = 1.0;
  conditions.density = 2.0;  // q = 1 exactly, so coefficients equal raw forces
  conditions.angleOfAttackDeg = alpha;
  conditions.referencePressure = referencePressure;
  conditions.reynoldsNumber = 500.0;
  return conditions;
}

FlowField constantField(const Mesh& mesh, double pressure, Vec2 velocity,
                        double viscosity = 1.0) {
  FlowField field;
  field.resize(mesh.cellCount());
  for (std::size_t c = 0; c < mesh.cellCount(); ++c) {
    field.pressure[c] = pressure;
    field.velocity[c] = velocity;
    field.density[c] = 2.0;
    field.viscosity[c] = viscosity;
  }
  return field;
}

AerodynamicForces forcesFrom(const FlowField& field, const FreestreamConditions& conditions) {
  auto surface = extractSurface(sharedMesh(), field, conditions, 1.0);
  EXPECT_TRUE(surface) << (surface.hasError() ? surface.error().format() : "");
  auto forces = integrateForces(surface.value(), conditions);
  EXPECT_TRUE(forces) << (forces.hasError() ? forces.error().format() : "");
  return std::move(forces).value();
}

// ---------------------------------------------------------------------------
// Exact identities
// ---------------------------------------------------------------------------

// The closed-contour identity, and the sharpest check available on the
// integration: the outward normals of a closed body sum to zero, so a pressure
// that is the same everywhere can push it in no direction at all. Any error in
// a normal, a face length or a sign shows up here and nowhere else.
TEST(Forces, UniformPressureExertsNoNetForce) {
  // Fluid at rest, so there is no shear either - the pressure term stands alone.
  const FlowField field = constantField(sharedMesh(), 7.5, Vec2{0.0, 0.0});
  const AerodynamicForces forces = forcesFrom(field, stream());

  EXPECT_NEAR(forces.force.pressure.x, 0.0, 1e-12);
  EXPECT_NEAR(forces.force.pressure.y, 0.0, 1e-12);
  EXPECT_NEAR(forces.moment.pressure, 0.0, 1e-12);
  EXPECT_NEAR(forces.liftCoefficient, 0.0, 1e-12);
  EXPECT_NEAR(forces.dragCoefficient, 0.0, 1e-12);
}

// And it holds whatever the reference level is, because the level cancels.
TEST(Forces, UniformPressureExertsNoNetForceAtAnyReferenceLevel) {
  const FlowField field = constantField(sharedMesh(), 1.0e5, Vec2{0.0, 0.0});
  const AerodynamicForces forces = forcesFrom(field, stream(0.0, 1.0e5));

  EXPECT_NEAR(forces.force.pressure.x, 0.0, 1e-9);
  EXPECT_NEAR(forces.force.pressure.y, 0.0, 1e-9);
}

// Fluid at rest exerts no shear, so nothing may leak from the friction term
// into the total.
TEST(Forces, StillFluidExertsNoFriction) {
  const FlowField field = constantField(sharedMesh(), 3.0, Vec2{0.0, 0.0});
  const AerodynamicForces forces = forcesFrom(field, stream());

  EXPECT_NEAR(forces.force.friction.x, 0.0, 1e-14);
  EXPECT_NEAR(forces.force.friction.y, 0.0, 1e-14);
  EXPECT_NEAR(forces.frictionDrag, 0.0, 1e-14);
}

// The split has to be a split: the parts must add up to the whole, in both
// axes and in the moment. Cheap to check and it catches a term counted twice.
TEST(Forces, PressureAndFrictionPartsSumToTheTotal) {
  const FlowField field = constantField(sharedMesh(), 2.0, Vec2{0.6, 0.2});
  const AerodynamicForces forces = forcesFrom(field, stream(5.0));

  EXPECT_NEAR(forces.drag, forces.pressureDrag + forces.frictionDrag, 1e-12);
  EXPECT_NEAR(forces.lift, forces.pressureLift + forces.frictionLift, 1e-12);
  EXPECT_NEAR(forces.pitchingMoment, -(forces.moment.pressure + forces.moment.friction),
              1e-12);

  // And the coefficients are the forces over the same scale.
  const double scale = forces.dynamicPressure * forces.chord;
  EXPECT_NEAR(forces.dragCoefficient, forces.drag / scale, 1e-12);
  EXPECT_NEAR(forces.liftCoefficient, forces.lift / scale, 1e-12);
}

// Lift and drag are the body-axis force rotated into wind axes, so their
// magnitude cannot depend on the incidence the rotation uses. Checking the
// invariant separately from the rotation catches a sign slip in either.
TEST(Forces, WindAxesPreserveTheMagnitudeOfTheBodyForce) {
  const FlowField field = constantField(sharedMesh(), 2.0, Vec2{0.6, 0.2});

  for (const double alpha : {-12.0, -3.0, 0.0, 7.5, 15.0}) {
    const AerodynamicForces forces = forcesFrom(field, stream(alpha));
    const Vec2 body = forces.force.total();
    const double bodyMagnitude = std::sqrt(body.x * body.x + body.y * body.y);
    const double windMagnitude =
        std::sqrt(forces.lift * forces.lift + forces.drag * forces.drag);
    EXPECT_NEAR(bodyMagnitude, windMagnitude, 1e-12) << "at alpha = " << alpha;
  }
}

// A force acting at the reference point produces no moment about it, and the
// same force further aft produces more. Rather than construct a field to do
// that, check the reference point enters the moment the way it must: shifting
// it by d changes the moment by d x F.
TEST(Forces, MovingTheMomentReferenceShiftsTheMomentByTheForceTimesTheArm) {
  const FlowField field = constantField(sharedMesh(), 2.0, Vec2{0.6, 0.2});
  auto surface = extractSurface(sharedMesh(), field, stream(4.0), 1.0);
  ASSERT_TRUE(surface);

  const Vec2 first{0.25, 0.0};
  const Vec2 second{0.75, 0.0};
  auto atFirst = integrateForces(surface.value(), stream(4.0), first);
  auto atSecond = integrateForces(surface.value(), stream(4.0), second);
  ASSERT_TRUE(atFirst);
  ASSERT_TRUE(atSecond);

  const Vec2 force = atFirst.value().force.total();
  const Vec2 shift = second - first;
  // M(b) = M(a) - (b - a) x F, in the counter-clockwise convention.
  const double expected =
      atFirst.value().moment.total() - (shift.x * force.y - shift.y * force.x);
  EXPECT_NEAR(atSecond.value().moment.total(), expected, 1e-12);
}

TEST(Forces, RejectsAnEmptyDistribution) {
  const SurfaceDistribution empty;
  const auto result = integrateForces(empty, stream());
  EXPECT_FALSE(result);
}

TEST(Forces, RejectsAFreestreamWithNoDynamicPressure) {
  const FlowField field = constantField(sharedMesh(), 1.0, Vec2{0.0, 0.0});
  auto surface = extractSurface(sharedMesh(), field, stream(), 1.0);
  ASSERT_TRUE(surface);

  FreestreamConditions still = stream();
  still.speed = 0.0;
  EXPECT_FALSE(integrateForces(surface.value(), still));
}

// ---------------------------------------------------------------------------
// Solved cases
// ---------------------------------------------------------------------------

/// A small C-grid, so the solved cases below stay quick.
const Mesh& solveMesh() {
  static const Mesh mesh = [] {
    auto section = cfd::geom::makeNaca4Digit(
        "0012", {.chord = 1.0, .trailingEdge = cfd::geom::TrailingEdge::Closed});
    EXPECT_TRUE(section);
    cfd::mesh::CGridOptions options = cfd::mesh::optionsFor(cfd::mesh::MeshResolution::Coarse);
    options.surfacePoints = 40;
    options.wakePoints = 20;
    options.normalPoints = 28;
    auto grid = cfd::mesh::generateCGrid(section.value(), options);
    EXPECT_TRUE(grid) << (grid.hasError() ? grid.error().format() : "");
    return std::move(grid).value();
  }();
  return mesh;
}

AerodynamicForces solvedForces(double alpha, int iterations) {
  const Mesh& mesh = solveMesh();
  const FreestreamConditions conditions = stream(alpha);

  auto faceConditions =
      cfd::flow::buildFaceConditions(mesh, cfd::flow::BoundaryConditions{}, conditions);
  EXPECT_TRUE(faceConditions);

  auto created = cfd::solver::SimpleSolver::create(mesh, faceConditions.value(),
                                                   cfd::solver::SimpleSettings{});
  EXPECT_TRUE(created);
  auto initial = FlowField::uniform(mesh.cellCount(), conditions, 1.0);
  EXPECT_TRUE(initial);
  EXPECT_TRUE(created.value().initialise(initial.value()));

  for (int i = 0; i < iterations; ++i) {
    const auto monitor = created.value().iterate();
    if (!std::isfinite(monitor.residuals.continuity)) {
      ADD_FAILURE() << "the solve diverged at iteration " << i;
      break;
    }
  }

  auto surface = extractSurface(mesh, created.value().field(), conditions, 1.0);
  EXPECT_TRUE(surface) << (surface.hasError() ? surface.error().format() : "");
  auto forces = integrateForces(surface.value(), conditions);
  EXPECT_TRUE(forces) << (forces.hasError() ? forces.error().format() : "");
  return std::move(forces).value();
}

// The headline symmetry check. A symmetric section in a stream that arrives
// straight on has nothing to distinguish up from down, so it can produce no
// lift and no pitching moment. Drag it must produce, because the fluid is
// viscous.
TEST(Forces, SymmetricSectionAtZeroIncidenceProducesNoLift) {
  const AerodynamicForces forces = solvedForces(0.0, 900);

  // Not to round-off: the discrete problem is only symmetric to the accuracy
  // the solve has reached, and Gauss-Seidel sweeps the cells in index order,
  // which is itself an asymmetric operation.
  EXPECT_NEAR(forces.liftCoefficient, 0.0, 5e-3);
  EXPECT_NEAR(forces.momentCoefficient, 0.0, 5e-3);

  // Drag at Re = 500 is large and unmistakably positive.
  EXPECT_GT(forces.dragCoefficient, 0.05);
  EXPECT_GT(forces.frictionDragCoefficient, 0.0);
}

// Lift must act in the direction the incidence is applied, and reversing the
// incidence on a symmetric section must reverse it exactly.
TEST(Forces, LiftFollowsTheSignOfIncidence) {
  const AerodynamicForces up = solvedForces(8.0, 900);
  const AerodynamicForces down = solvedForces(-8.0, 900);

  EXPECT_GT(up.liftCoefficient, 0.0);
  EXPECT_LT(down.liftCoefficient, 0.0);

  // A symmetric section is its own mirror image, so the two must be opposite.
  EXPECT_NEAR(up.liftCoefficient, -down.liftCoefficient, 1e-2);
  // Drag, being along the stream, is unchanged by the reflection.
  EXPECT_NEAR(up.dragCoefficient, down.dragCoefficient, 1e-2);
}

// More incidence, more lift - the property that makes the number a lift
// coefficient rather than an arbitrary integral.
TEST(Forces, LiftGrowsWithIncidence) {
  const AerodynamicForces low = solvedForces(2.0, 900);
  const AerodynamicForces high = solvedForces(8.0, 900);

  EXPECT_GT(low.liftCoefficient, 0.0);
  EXPECT_GT(high.liftCoefficient, low.liftCoefficient);
  // And efficiency is a real, finite number.
  EXPECT_TRUE(high.hasLiftToDrag());
  EXPECT_GT(high.liftToDrag(), 0.0);
}

// Force convergence: the coefficients must settle as the solve does. A force
// that is still moving after the residuals have fallen is not a force, it is
// whatever the iteration happened to be holding.
TEST(Forces, CoefficientsSettleAsTheSolveConverges) {
  const AerodynamicForces early = solvedForces(8.0, 700);
  const AerodynamicForces late = solvedForces(8.0, 1400);

  // Twice the iterations must not move the answer appreciably.
  EXPECT_NEAR(early.liftCoefficient, late.liftCoefficient, 2e-2);
  EXPECT_NEAR(early.dragCoefficient, late.dragCoefficient, 2e-2);
  EXPECT_NEAR(early.momentCoefficient, late.momentCoefficient, 2e-2);
}

}  // namespace
