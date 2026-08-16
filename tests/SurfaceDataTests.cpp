// Tests for the surface quantities and for separation detection.
//
// Most of these impose a known field on a real aerofoil mesh rather than
// solving one, so the extraction is tested on its own and exactly. A single
// case at the end runs the solver, because separation is a property of a
// computed flow and cannot be checked any other way.

#include "cfd/post/SurfaceData.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>

#include "cfd/geom/Airfoil.hpp"
#include "cfd/mesh/CGrid.hpp"
#include "cfd/solver/SimpleSolver.hpp"

namespace {

using cfd::Vec2;
using cfd::flow::FlowField;
using cfd::flow::FreestreamConditions;
using cfd::mesh::Mesh;
using cfd::post::extractSurface;
using cfd::post::SurfaceDistribution;
using cfd::post::SurfacePoint;

/// A coarse C-grid around NACA 0012, shared by every test here.
const Mesh& sharedMesh() {
  static const Mesh mesh = [] {
    auto section = cfd::geom::makeNaca4Digit(
        "0012", {.chord = 1.0, .trailingEdge = cfd::geom::TrailingEdge::Closed});
    EXPECT_TRUE(section);
    cfd::mesh::CGridOptions options = cfd::mesh::optionsFor(cfd::mesh::MeshResolution::Coarse);
    auto grid = cfd::mesh::generateCGrid(section.value(), options);
    EXPECT_TRUE(grid) << (grid.hasError() ? grid.error().format() : "");
    return std::move(grid).value();
  }();
  return mesh;
}

FreestreamConditions stream(double alpha = 0.0) {
  FreestreamConditions conditions;
  conditions.speed = 1.0;
  conditions.density = 2.0;  // so q = 1 exactly, making Cp easy to reason about
  conditions.angleOfAttackDeg = alpha;
  conditions.referencePressure = 0.0;
  conditions.reynoldsNumber = 500.0;
  return conditions;
}

/// A field with a chosen constant pressure and velocity everywhere.
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

SurfaceDistribution extract(const FlowField& field, const FreestreamConditions& conditions) {
  auto result = extractSurface(sharedMesh(), field, conditions, 1.0);
  EXPECT_TRUE(result) << (result.hasError() ? result.error().format() : "");
  return std::move(result).value();
}

// ---------------------------------------------------------------------------
// Geometry of the extraction
// ---------------------------------------------------------------------------

TEST(SurfaceData, CoversBothSurfacesFromTheLeadingEdge) {
  const SurfaceDistribution surface =
      extract(constantField(sharedMesh(), 0.0, Vec2{1.0, 0.0}), stream());

  ASSERT_FALSE(surface.empty());
  EXPECT_GT(surface.upper.size(), 10u);
  EXPECT_GT(surface.lower.size(), 10u);

  // Both start at the nose and finish at the tail.
  for (const auto* side : {&surface.upper, &surface.lower}) {
    EXPECT_NEAR(0.0, side->front().chordFraction, 0.01);
    EXPECT_NEAR(1.0, side->back().chordFraction, 0.01);
    EXPECT_DOUBLE_EQ(0.0, side->front().arcLength);
  }
}

TEST(SurfaceData, ArcLengthIncreasesAlongEachSurface) {
  const SurfaceDistribution surface =
      extract(constantField(sharedMesh(), 0.0, Vec2{1.0, 0.0}), stream());

  for (const auto* side : {&surface.upper, &surface.lower}) {
    for (std::size_t i = 1; i < side->size(); ++i) {
      EXPECT_GT((*side)[i].arcLength, (*side)[i - 1].arcLength) << "at " << i;
    }
    // Half the section's perimeter, give or take.
    EXPECT_GT(side->back().arcLength, 0.9);
    EXPECT_LT(side->back().arcLength, 1.2);
  }
}

TEST(SurfaceData, NormalsPointIntoTheFluidAndTangentsAreUnit) {
  const SurfaceDistribution surface =
      extract(constantField(sharedMesh(), 0.0, Vec2{1.0, 0.0}), stream());

  for (const auto* side : {&surface.upper, &surface.lower}) {
    for (const SurfacePoint& point : *side) {
      EXPECT_NEAR(1.0, length(point.normal), 1e-12);
      EXPECT_NEAR(1.0, length(point.tangent), 1e-12);
      // A normal into the fluid is perpendicular to the surface it leaves.
      EXPECT_NEAR(0.0, dot(point.normal, point.tangent), 1e-9);
    }
  }
  // The upper surface's outward normal points up, the lower's points down.
  EXPECT_GT(surface.upper[surface.upper.size() / 2].normal.y, 0.0);
  EXPECT_LT(surface.lower[surface.lower.size() / 2].normal.y, 0.0);
}

// ---------------------------------------------------------------------------
// Pressure coefficient
// ---------------------------------------------------------------------------

// Cp is the surface pressure measured in units of dynamic pressure, so a field
// at freestream pressure has Cp = 0 and one a full q above it has Cp = 1.
TEST(SurfaceData, PressureCoefficientIsPressureOverDynamicPressure) {
  const FreestreamConditions conditions = stream();
  ASSERT_DOUBLE_EQ(1.0, conditions.dynamicPressure());  // rho = 2, U = 1

  const SurfaceDistribution atFreestream =
      extract(constantField(sharedMesh(), 0.0, Vec2{}), conditions);
  for (const SurfacePoint& point : atFreestream.upper) {
    EXPECT_NEAR(0.0, point.pressureCoefficient, 1e-12);
  }

  const SurfaceDistribution atStagnation =
      extract(constantField(sharedMesh(), 1.0, Vec2{}), conditions);
  for (const SurfacePoint& point : atStagnation.upper) {
    EXPECT_NEAR(1.0, point.pressureCoefficient, 1e-12);
  }
}

TEST(SurfaceData, PressureCoefficientTracksTheReferencePressure) {
  FreestreamConditions conditions = stream();
  conditions.referencePressure = 500.0;

  // Only the difference from freestream matters.
  const SurfaceDistribution surface =
      extract(constantField(sharedMesh(), 501.0, Vec2{}), conditions);
  for (const SurfacePoint& point : surface.upper) {
    EXPECT_NEAR(1.0, point.pressureCoefficient, 1e-12);
  }
}

// ---------------------------------------------------------------------------
// Wall shear
// ---------------------------------------------------------------------------

TEST(SurfaceData, WallShearVanishesWhenTheFluidIsAtRest) {
  const SurfaceDistribution surface =
      extract(constantField(sharedMesh(), 0.0, Vec2{0.0, 0.0}), stream());

  for (const auto* side : {&surface.upper, &surface.lower}) {
    for (const SurfacePoint& point : *side) {
      EXPECT_NEAR(0.0, point.wallShear, 1e-15);
      EXPECT_NEAR(0.0, point.nearWallSpeed, 1e-15);
      EXPECT_FALSE(point.reversed);
    }
  }
  EXPECT_FALSE(surface.hasSeparation());
}

// tau_w = mu * u_t / d, with u_t the wall-parallel speed in the first cell and
// d the perpendicular distance to the wall. Checked directly against the mesh.
TEST(SurfaceData, WallShearIsTheNearWallVelocityGradient) {
  const Mesh& mesh = sharedMesh();
  constexpr double kViscosity = 3.0;
  const FlowField field = constantField(mesh, 0.0, Vec2{1.0, 0.0}, kViscosity);
  const SurfaceDistribution surface = extract(field, stream());

  // Take a station on the upper surface, away from the ends.
  const SurfacePoint& point = surface.upper[surface.upper.size() / 2];

  // Recover the owning cell by matching the face centre.
  std::size_t owner = 0;
  double best = 1e30;
  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    if (mesh.faces()[f].boundary != cfd::mesh::BoundaryType::Wall) {
      continue;
    }
    const double gap = distance(mesh.faceCentres()[f], point.position);
    if (gap < best) {
      best = gap;
      owner = static_cast<std::size_t>(mesh.faces()[f].owner);
    }
  }
  ASSERT_LT(best, 1e-12);

  const Vec2 offset = mesh.cellCentroids()[owner] - point.position;
  const double wallDistance = std::abs(dot(offset, point.normal));
  const Vec2 velocity{1.0, 0.0};
  const Vec2 parallel = velocity - point.normal * dot(velocity, point.normal);

  const double expected = kViscosity * dot(parallel, point.tangent) / wallDistance;
  EXPECT_NEAR(expected, point.wallShear, 1e-9 * std::abs(expected));
  EXPECT_NEAR(point.wallShear / stream().dynamicPressure(), point.skinFriction, 1e-12);
}

// A stream running backwards must register as reversed everywhere it is not
// being confused by the dividing point.
TEST(SurfaceData, ReversedFlowIsFlaggedAsReversed) {
  const SurfaceDistribution forward =
      extract(constantField(sharedMesh(), 0.0, Vec2{1.0, 0.0}), stream());
  const SurfaceDistribution backward =
      extract(constantField(sharedMesh(), 0.0, Vec2{-1.0, 0.0}), stream());

  std::size_t forwardReversed = 0;
  std::size_t backwardReversed = 0;
  for (const SurfacePoint& point : forward.upper) {
    forwardReversed += point.reversed ? 1 : 0;
  }
  for (const SurfacePoint& point : backward.upper) {
    backwardReversed += point.reversed ? 1 : 0;
  }
  EXPECT_GT(backwardReversed, forwardReversed);
  EXPECT_GT(backwardReversed, backward.upper.size() / 2);
}

// ---------------------------------------------------------------------------
// Validation and error paths
// ---------------------------------------------------------------------------

TEST(SurfaceData, RejectsAFieldThatDoesNotMatchTheMesh) {
  FlowField wrong;
  wrong.resize(5);
  EXPECT_TRUE(extractSurface(sharedMesh(), wrong, stream(), 1.0).hasError());
}

TEST(SurfaceData, RejectsUnphysicalFreestreamConditions) {
  FreestreamConditions bad = stream();
  bad.speed = -1.0;
  EXPECT_TRUE(
      extractSurface(sharedMesh(), constantField(sharedMesh(), 0.0, Vec2{}), bad, 1.0)
          .hasError());
}

// ---------------------------------------------------------------------------
// A solved flow
// ---------------------------------------------------------------------------

/// A deliberately small C-grid, so the two solved cases below stay quick.
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

/// Solve the section at the given incidence and extract its surface.
SurfaceDistribution solved(double alpha, int iterations) {
  const Mesh& mesh = solveMesh();
  FreestreamConditions conditions = stream(alpha);

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

  auto result = extractSurface(mesh, created.value().field(), conditions, 1.0);
  EXPECT_TRUE(result) << (result.hasError() ? result.error().format() : "");
  return std::move(result).value();
}

// The headline check. A symmetric section at zero incidence must produce a
// symmetric solution, and separation must be reported only where the computed
// wall shear actually reverses - not because a criterion was written in.
TEST(SurfaceData, SymmetricSectionAtZeroIncidenceIsSymmetricAndAttached) {
  const SurfaceDistribution surface = solved(0.0, 900);

  // Splitting at the leading edge node rather than a face gives the two
  // surfaces the same number of stations, each the mirror of the other.
  ASSERT_EQ(surface.upper.size(), surface.lower.size());

  // Mirror symmetry: the two surfaces must agree station for station.
  for (std::size_t i = 0; i < surface.lower.size(); ++i) {
    // Not to round-off: Gauss-Seidel sweeps the cells in index order, which
    // is itself asymmetric, so a partially converged symmetric problem carries
    // a little asymmetry with it. A thousandth is far tighter than any
    // physically meaningful difference.
    EXPECT_NEAR(surface.upper[i].pressureCoefficient, surface.lower[i].pressureCoefficient,
                1e-3)
        << "Cp differs at station " << i;
    EXPECT_NEAR(surface.upper[i].skinFriction, surface.lower[i].skinFriction, 1e-3)
        << "Cf differs at station " << i;
  }

  // Highest pressure is at the nose, where the flow stops.
  EXPECT_LT(std::abs(surface.stagnationChordFraction), 0.02);
  EXPECT_GT(surface.maxPressureCoefficient, 0.8);

  // Attached: nothing reversed anywhere.
  for (const auto* side : {&surface.upper, &surface.lower}) {
    for (const SurfacePoint& point : *side) {
      EXPECT_FALSE(point.reversed) << "reversed at x/c=" << point.chordFraction;
    }
  }
  EXPECT_FALSE(surface.hasSeparation());
}

// At incidence the flow divides further aft along the lower surface, and the
// suction side separates. Both come out of the solution.
TEST(SurfaceData, IncidenceMovesTheStagnationPointAndSeparatesTheSuctionSide) {
  const SurfaceDistribution level = solved(0.0, 900);
  const SurfaceDistribution pitched = solved(12.0, 900);

  // The stagnation point slides aft as incidence increases.
  EXPECT_GT(pitched.stagnationChordFraction, level.stagnationChordFraction);

  // Suction side separates; the pressure side stays attached.
  EXPECT_TRUE(pitched.upperSeparation.found) << "expected separation on the suction side";
  EXPECT_GT(pitched.upperSeparation.chordFraction, 0.0);
  EXPECT_LT(pitched.upperSeparation.chordFraction, 1.0);
  EXPECT_FALSE(pitched.lowerSeparation.found)
      << "the pressure side should not separate at this incidence";

  // Suction: the upper surface reaches a lower minimum pressure than at zero
  // incidence, which is where lift comes from.
  EXPECT_LT(pitched.minPressureCoefficient, level.minPressureCoefficient);
}

}  // namespace
