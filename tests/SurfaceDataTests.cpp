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
#include "cfd/post/Streamlines.hpp"
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

/// A field whose speed varies as a*d + b*d^2 with distance d from the wall,
/// directed along the surface.
///
/// The wall gradient of that profile is exactly `a`, whatever `b` is - which is
/// what makes it a real test of the wall-shear estimate rather than a
/// restatement of the formula used to compute it. A one-sided first difference
/// returns a + b*d1 and is wrong by b*d1; a parabola fitted through the wall
/// and the first two cells returns a exactly.
FlowField wallProfileField(const Mesh& mesh, double a, double b, double viscosity) {
  struct WallFace {
    Vec2 centre;
    Vec2 normal;   // into the fluid
    Vec2 tangent;  // along the surface
  };
  std::vector<WallFace> walls;
  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    if (mesh.faces()[f].boundary != cfd::mesh::BoundaryType::Wall) {
      continue;
    }
    const Vec2& p0 = mesh.nodes()[static_cast<std::size_t>(mesh.faces()[f].nodes[0])];
    const Vec2& p1 = mesh.nodes()[static_cast<std::size_t>(mesh.faces()[f].nodes[1])];
    const Vec2 along = p1 - p0;
    const double len = length(along);
    if (!(len > 0.0)) {
      continue;
    }
    walls.push_back({mesh.faceCentres()[f], mesh.faceNormals()[f] * -1.0, along * (1.0 / len)});
  }
  EXPECT_FALSE(walls.empty());

  FlowField field;
  field.resize(mesh.cellCount());
  for (std::size_t c = 0; c < mesh.cellCount(); ++c) {
    const Vec2& centroid = mesh.cellCentroids()[c];

    // Nearest wall face. Linear in the wall count, which is nothing here, and
    // it means the profile is a function of wall distance on any mesh rather
    // than relying on how the grid happens to be indexed.
    std::size_t nearest = 0;
    double best = 1e30;
    for (std::size_t w = 0; w < walls.size(); ++w) {
      const double gap = distance(centroid, walls[w].centre);
      if (gap < best) {
        best = gap;
        nearest = w;
      }
    }

    const WallFace& wall = walls[nearest];
    const double d = std::abs(dot(centroid - wall.centre, wall.normal));
    const double speed = a * d + b * d * d;

    field.pressure[c] = 0.0;
    field.velocity[c] = wall.tangent * speed;
    field.density[c] = 2.0;
    field.viscosity[c] = viscosity;
  }
  return field;
}

// A profile that is linear in wall distance has a gradient every scheme should
// recover exactly, so this pins the plumbing: the normal direction, the
// distance, the viscosity and the tangential projection.
TEST(SurfaceData, WallShearIsExactForALinearProfile) {
  constexpr double kViscosity = 3.0;
  constexpr double kSlope = 7.0;
  const FlowField field = wallProfileField(sharedMesh(), kSlope, 0.0, kViscosity);
  const SurfaceDistribution surface = extract(field, stream());

  // Away from the ends, where the surfaces meet and the tangent is ambiguous.
  for (std::size_t i = surface.upper.size() / 4; i < 3 * surface.upper.size() / 4; ++i) {
    const SurfacePoint& point = surface.upper[i];
    EXPECT_NEAR(std::abs(point.wallShear), kViscosity * kSlope, 1e-6 * kViscosity * kSlope)
        << "at x/c = " << point.chordFraction;
  }
}

// The one that distinguishes the two schemes. With a strong quadratic term the
// one-sided difference is wrong by b*d1, which at the first-layer height here
// is of the same order as the answer itself; the parabola fit is exact.
TEST(SurfaceData, WallShearIsSecondOrderAccurate) {
  constexpr double kViscosity = 1.0;
  constexpr double kSlope = 1.0;
  constexpr double kCurvature = 1000.0;
  const FlowField field = wallProfileField(sharedMesh(), kSlope, kCurvature, kViscosity);
  const SurfaceDistribution surface = extract(field, stream());

  for (std::size_t i = surface.upper.size() / 4; i < 3 * surface.upper.size() / 4; ++i) {
    const SurfacePoint& point = surface.upper[i];
    // 1% rather than round-off: the two cells either side of a station sit on a
    // curved, graded grid, so "distance from the wall" is not perfectly the
    // same coordinate for both. Being within 1% of the exact slope while a
    // first difference would be tens of percent out is the claim.
    EXPECT_NEAR(std::abs(point.wallShear), kViscosity * kSlope, 0.01 * kViscosity * kSlope)
        << "at x/c = " << point.chordFraction;
  }
}

TEST(SurfaceData, SkinFrictionIsWallShearOverDynamicPressure) {
  const FlowField field = wallProfileField(sharedMesh(), 5.0, 0.0, 2.0);
  const SurfaceDistribution surface = extract(field, stream());
  const SurfacePoint& point = surface.upper[surface.upper.size() / 2];
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

/// Solve the section at the given incidence, starting from `start` if given and
/// from the undisturbed stream otherwise. Returns the solved field.
FlowField solveField(double alpha, int iterations, const FlowField* start) {
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
  if (start != nullptr) {
    // Carry the solved quantities over, exactly as the application does when
    // the freestream changes: density and viscosity still come from the new
    // conditions, only velocity and pressure are inherited.
    EXPECT_EQ(start->size(), initial.value().size());
    initial.value().velocity = start->velocity;
    initial.value().pressure = start->pressure;
  }
  EXPECT_TRUE(created.value().initialise(initial.value()));

  for (int i = 0; i < iterations; ++i) {
    const auto monitor = created.value().iterate();
    if (!std::isfinite(monitor.residuals.continuity)) {
      ADD_FAILURE() << "the solve diverged at iteration " << i;
      break;
    }
  }
  return created.value().field();
}

/// Solve the section at the given incidence and extract its surface.
SurfaceDistribution solved(double alpha, int iterations) {
  const FlowField field = solveField(alpha, iterations, nullptr);
  auto result = extractSurface(solveMesh(), field, stream(alpha), 1.0);
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

// Sweeping incidence in the application continues from the field already on
// screen rather than starting cold, which is the only thing that makes a slider
// usable: a nudge would otherwise throw away every iteration so far.
//
// That is only legitimate if the steady solution is independent of what it was
// started from. It should be - nothing in a converged steady answer remembers
// the guess - but "should be" is exactly the kind of claim worth pinning down,
// because if it were false the slider would quietly produce history-dependent
// results that still looked plausible.
TEST(SurfaceData, ContinuingFromANeighbouringIncidenceReachesTheSameSolution) {
  const Mesh& mesh = solveMesh();

  // Cold: straight to 12 degrees from the undisturbed stream.
  const FlowField cold = solveField(12.0, 900, nullptr);

  // Continued: 6 degrees first, then carry that field into the 12 degree case
  // and give it the same number of iterations again.
  const FlowField intermediate = solveField(6.0, 900, nullptr);
  const FlowField continued = solveField(12.0, 900, &intermediate);

  auto coldSurface = extractSurface(mesh, cold, stream(12.0), 1.0);
  auto continuedSurface = extractSurface(mesh, continued, stream(12.0), 1.0);
  ASSERT_TRUE(coldSurface);
  ASSERT_TRUE(continuedSurface);
  ASSERT_EQ(coldSurface.value().upper.size(), continuedSurface.value().upper.size());

  // Both routes must arrive at the same surface loading. The tolerance is set
  // by how far either is from full convergence at 900 iterations, not by any
  // difference the starting field is expected to leave behind.
  for (std::size_t i = 0; i < coldSurface.value().upper.size(); ++i) {
    EXPECT_NEAR(coldSurface.value().upper[i].pressureCoefficient,
                continuedSurface.value().upper[i].pressureCoefficient, 5e-3)
        << "upper Cp differs at station " << i;
    EXPECT_NEAR(coldSurface.value().upper[i].skinFriction,
                continuedSurface.value().upper[i].skinFriction, 5e-3)
        << "upper Cf differs at station " << i;
  }

  // And they must agree on the thing the surface panel actually reports.
  EXPECT_EQ(coldSurface.value().upperSeparation.found,
            continuedSurface.value().upperSeparation.found);
  if (coldSurface.value().upperSeparation.found) {
    EXPECT_NEAR(coldSurface.value().upperSeparation.chordFraction,
                continuedSurface.value().upperSeparation.chordFraction, 0.02);
  }
  EXPECT_NEAR(coldSurface.value().stagnationChordFraction,
              continuedSurface.value().stagnationChordFraction, 0.02);
}


// ---------------------------------------------------------------------------
// Streamlines
// ---------------------------------------------------------------------------

TEST(Streamlines, LocatesTheCellContainingAPoint) {
  const Mesh& mesh = sharedMesh();

  // Every cell centroid must be found inside its own cell.
  for (std::size_t c = 0; c < mesh.cellCount(); c += 97) {
    const int found = cfd::post::locateCell(mesh, mesh.cellCentroids()[c], -1);
    EXPECT_EQ(static_cast<int>(c), found) << "centroid of cell " << c;
  }

  // A point far outside the domain belongs to nothing.
  EXPECT_EQ(-1, cfd::post::locateCell(mesh, Vec2{1e6, 1e6}, -1));
}

// Walking is only asked to travel a short way, which is how it is used: the
// hint is the cell the previous step ended in. A hint on the far side of the
// section cannot work, because the walk would have to pass through the solid.
TEST(Streamlines, WalkingFromANearbyHintMatchesASearch) {
  const Mesh& mesh = sharedMesh();

  for (std::size_t c = 5; c < mesh.cellCount(); c += 313) {
    const Vec2 point = mesh.cellCentroids()[c];

    // Start from a face neighbour, as a tracer would.
    int hint = static_cast<int>(c);
    for (const int faceIndex : mesh.cellFaces()[c]) {
      const int across = cfd::mesh::oppositeCell(mesh, static_cast<std::size_t>(faceIndex));
      if (across >= 0 && across != static_cast<int>(c)) {
        hint = across;
        break;
      }
    }
    EXPECT_EQ(static_cast<int>(c), cfd::post::locateCell(mesh, point, hint)) << "cell " << c;
  }
}

// In a uniform stream every streamline is a straight line along it. Anything
// else means the integrator, not the flow, is bending the path.
TEST(Streamlines, AreStraightInAUniformField) {
  const Mesh& mesh = sharedMesh();
  const FlowField field = constantField(mesh, 0.0, Vec2{1.0, 0.0});

  cfd::post::StreamlineOptions options;
  options.referenceSpeed = 1.0;
  options.seeds = {Vec2{-4.0, 2.0}, Vec2{-4.0, -3.0}};
  options.maxSteps = 500;

  auto result = cfd::post::traceStreamlines(mesh, field, options);
  ASSERT_TRUE(result) << (result.hasError() ? result.error().format() : "");
  ASSERT_EQ(2u, result.value().size());

  for (const auto& line : result.value()) {
    ASSERT_GT(line.size(), 10u);
    const double y = line.front().y;
    double previousX = line.front().x;
    for (const Vec2& point : line) {
      EXPECT_NEAR(y, point.y, 1e-9) << "the path should not drift across the stream";
      EXPECT_GE(point.x, previousX - 1e-12) << "the path should not double back";
      previousX = point.x;
    }
  }
}

TEST(Streamlines, SkipSeedsOutsideTheDomain) {
  const Mesh& mesh = sharedMesh();
  cfd::post::StreamlineOptions options;
  options.seeds = {Vec2{1e5, 1e5}};
  auto result = cfd::post::traceStreamlines(mesh, constantField(mesh, 0.0, Vec2{1.0, 0.0}), options);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result.value().empty());
}

TEST(Streamlines, RejectAMismatchedField) {
  FlowField wrong;
  wrong.resize(3);
  cfd::post::StreamlineOptions options;
  options.seeds = {Vec2{0.0, 0.0}};
  EXPECT_TRUE(cfd::post::traceStreamlines(sharedMesh(), wrong, options).hasError());
}

}  // namespace