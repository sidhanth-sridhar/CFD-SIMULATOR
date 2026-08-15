// Validation of the generated C-grid around a real section.
//
// The container tests in MeshTests.cpp already cover connectivity and metrics
// on trivial blocks. What is checked here is that the *generated* grid is a
// usable computational domain: it wraps the aerofoil, it does not fold over,
// its boundaries are the ones a solver will need, and refining it behaves.

#include "cfd/mesh/CGrid.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <string>
#include <vector>

#include "cfd/geom/Airfoil.hpp"
#include "cfd/mesh/Mesh.hpp"

namespace {

using cfd::Vec2;
using cfd::geom::Airfoil;
using cfd::geom::AirfoilOptions;
using cfd::geom::makeNaca4Digit;
using cfd::geom::TrailingEdge;
using cfd::mesh::BoundaryType;
using cfd::mesh::CGridOptions;
using cfd::mesh::Face;
using cfd::mesh::generateCGrid;
using cfd::mesh::Mesh;
using cfd::mesh::MeshResolution;
using cfd::mesh::optionsFor;

Airfoil section(const std::string& designation = "2412", double chord = 1.0) {
  auto result = makeNaca4Digit(
      designation, AirfoilOptions{.chord = chord, .trailingEdge = TrailingEdge::Closed});
  EXPECT_TRUE(result) << (result.hasError() ? result.error().format() : "");
  return std::move(result).value();
}

Mesh meshFor(MeshResolution resolution, const std::string& designation = "2412") {
  auto result = generateCGrid(section(designation), optionsFor(resolution));
  EXPECT_TRUE(result) << (result.hasError() ? result.error().format() : "");
  return std::move(result).value();
}

Mesh meshWith(const CGridOptions& options, const std::string& designation = "2412") {
  auto result = generateCGrid(section(designation), options);
  EXPECT_TRUE(result) << (result.hasError() ? result.error().format() : "");
  return std::move(result).value();
}

/// Exact area of the fluid domain: a rectangle behind the trailing edge plus a
/// half ellipse round the front, less the area the section occupies.
double analyticDomainArea(const CGridOptions& options, const Airfoil& foil) {
  const double c = foil.chord();
  const double xTrailing = foil.trailingEdge().x;
  const double halfHeight = options.verticalChords * c;
  const double frontRadius = xTrailing + options.upstreamChords * c;

  const double rectangle = 2.0 * halfHeight * options.downstreamChords * c;
  const double halfEllipse = 0.5 * std::numbers::pi * frontRadius * halfHeight;
  return rectangle + halfEllipse - foil.area();
}

const std::vector<MeshResolution> kResolutions{MeshResolution::Coarse, MeshResolution::Medium,
                                               MeshResolution::Fine};

// ---------------------------------------------------------------------------
// The headline requirement: a valid mesh
// ---------------------------------------------------------------------------

TEST(CGrid, EveryCellHasPositiveArea) {
  for (const MeshResolution resolution : kResolutions) {
    const Mesh mesh = meshFor(resolution);

    std::size_t nonPositive = 0;
    double worst = 0.0;
    for (const double area : mesh.cellAreas()) {
      if (!(area > 0.0)) {
        ++nonPositive;
        worst = std::min(worst, area);
      }
    }
    EXPECT_EQ(0u, nonPositive)
        << toString(resolution) << ": " << nonPositive << " non-positive cells, worst " << worst;
  }
}

TEST(CGrid, ContainsNoInvertedCells) {
  for (const MeshResolution resolution : kResolutions) {
    const Mesh mesh = meshFor(resolution);
    EXPECT_EQ(0u, mesh.quality().invertedCells) << toString(resolution);
    EXPECT_GT(mesh.quality().minCellArea, 0.0) << toString(resolution);
  }
}

// Cambered and symmetric sections stress the trailing-edge corner differently.
TEST(CGrid, StaysValidAcrossSections) {
  for (const std::string& designation : {"0012", "2412", "4412", "0006", "6409"}) {
    const Mesh mesh = meshFor(MeshResolution::Medium, designation);
    EXPECT_EQ(0u, mesh.quality().invertedCells) << designation;
  }
}

TEST(CGrid, SatisfiesEulersFormula) {
  const Mesh mesh = meshFor(MeshResolution::Coarse);

  const auto v = static_cast<long long>(mesh.nodeCount());
  const auto e = static_cast<long long>(mesh.faceCount());
  const auto f = static_cast<long long>(mesh.cellCount()) + 1;
  EXPECT_EQ(2, v - e + f);
}

TEST(CGrid, AreaVectorsCloseAroundEveryCell) {
  const Mesh mesh = meshFor(MeshResolution::Coarse);

  for (std::size_t c = 0; c < mesh.cellCount(); ++c) {
    Vec2 sum{0.0, 0.0};
    double scale = 0.0;
    for (const int f : mesh.cellFaces()[c]) {
      const auto face = static_cast<std::size_t>(f);
      const double sign = (mesh.faces()[face].owner == static_cast<int>(c)) ? 1.0 : -1.0;
      sum += mesh.faceNormals()[face] * (mesh.faceAreas()[face] * sign);
      scale += mesh.faceAreas()[face];
    }
    ASSERT_LT(length(sum), 1e-12 * scale) << "cell " << c;
  }
}

TEST(CGrid, NormalsAreConsistentlyOriented) {
  const Mesh mesh = meshFor(MeshResolution::Coarse);

  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    const Face& face = mesh.faces()[f];
    ASSERT_NEAR(1.0, length(mesh.faceNormals()[f]), 1e-12) << "face " << f;

    if (face.isInterior()) {
      const Vec2 link = mesh.cellCentroids()[static_cast<std::size_t>(face.neighbour)] -
                        mesh.cellCentroids()[static_cast<std::size_t>(face.owner)];
      ASSERT_GT(dot(mesh.faceNormals()[f], link), 0.0) << "interior face " << f;
    } else {
      const Vec2 outward =
          mesh.faceCentres()[f] - mesh.cellCentroids()[static_cast<std::size_t>(face.owner)];
      ASSERT_GT(dot(mesh.faceNormals()[f], outward), 0.0) << "boundary face " << f;
    }
  }
}

// ---------------------------------------------------------------------------
// Boundaries
// ---------------------------------------------------------------------------

TEST(CGrid, BoundaryFacesAreIdentified) {
  const CGridOptions options = optionsFor(MeshResolution::Medium);
  const Mesh mesh = meshWith(options);

  const std::size_t wall = mesh.countFaces(BoundaryType::Wall);
  const std::size_t wake = mesh.countFaces(BoundaryType::WakeCut);
  const std::size_t farfield = mesh.countFaces(BoundaryType::Farfield);
  const std::size_t outlet = mesh.countFaces(BoundaryType::Outlet);

  // The wall carries one face per segment of the section's closed contour.
  EXPECT_EQ(static_cast<std::size_t>(2 * options.surfacePoints - 2), wall);
  // Both sides of the cut.
  EXPECT_EQ(static_cast<std::size_t>(2 * options.wakePoints), wake);
  // The outer C, one face per interval along i.
  EXPECT_EQ(static_cast<std::size_t>(mesh.nodesI() - 1), farfield);
  // The two downstream planes.
  EXPECT_EQ(static_cast<std::size_t>(2 * (mesh.nodesJ() - 1)), outlet);

  EXPECT_EQ(mesh.faceCount(),
            wall + wake + farfield + outlet + mesh.countFaces(BoundaryType::Interior));
}

TEST(CGrid, WallFacesLieOnTheSection) {
  const Airfoil foil = section();
  const CGridOptions options = optionsFor(MeshResolution::Medium);
  const Mesh mesh = meshWith(options);

  double wallLength = 0.0;
  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    if (mesh.faces()[f].boundary != BoundaryType::Wall) {
      continue;
    }
    wallLength += mesh.faceAreas()[f];

    // Every wall face centre must sit inside the section's bounding box.
    const Vec2& centre = mesh.faceCentres()[f];
    const auto [lower, upper] = foil.bounds();
    EXPECT_GE(centre.x, lower.x - 1e-9);
    EXPECT_LE(centre.x, upper.x + 1e-9);
    EXPECT_GE(centre.y, lower.y - 1e-9);
    EXPECT_LE(centre.y, upper.y + 1e-9);
  }

  // The wall faces are the section's contour, so their total length is its
  // perimeter.
  EXPECT_NEAR(foil.perimeter(), wallLength, 2e-3 * foil.perimeter());
}

TEST(CGrid, FarfieldAndOutletSitAtTheRequestedExtents) {
  CGridOptions options = optionsFor(MeshResolution::Coarse);
  options.upstreamChords = 11.0;
  options.downstreamChords = 22.0;
  options.verticalChords = 13.0;

  const Airfoil foil = section();
  const Mesh mesh = meshWith(options);
  const double xTrailing = foil.trailingEdge().x;

  double mostUpstream = 0.0;
  double mostVertical = 0.0;
  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    if (mesh.faces()[f].boundary != BoundaryType::Farfield) {
      continue;
    }
    const Vec2& centre = mesh.faceCentres()[f];
    mostUpstream = std::min(mostUpstream, centre.x);
    mostVertical = std::max(mostVertical, std::abs(centre.y));
  }
  EXPECT_NEAR(-options.upstreamChords, mostUpstream, 0.05);
  EXPECT_NEAR(options.verticalChords, mostVertical, 1e-9);

  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    if (mesh.faces()[f].boundary != BoundaryType::Outlet) {
      continue;
    }
    EXPECT_NEAR(xTrailing + options.downstreamChords, mesh.faceCentres()[f].x, 1e-9);
  }
}

// The wake cut is a slit, not a wall: the two sides lie on top of each other,
// face opposite ways, and a solver must join them rather than apply a boundary
// condition. Each face therefore has to find its partner.
TEST(CGrid, WakeCutFacesArePairedAndCoincident) {
  const Mesh mesh = meshFor(MeshResolution::Medium);

  std::size_t paired = 0;
  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    const Face& face = mesh.faces()[f];
    if (face.boundary != BoundaryType::WakeCut) {
      EXPECT_EQ(-1, face.partner) << "only wake cut faces have partners";
      continue;
    }
    ASSERT_GE(face.partner, 0) << "wake cut face " << f << " has no partner";
    ++paired;

    const auto partner = static_cast<std::size_t>(face.partner);
    // Pairing is symmetric.
    EXPECT_EQ(static_cast<int>(f), mesh.faces()[partner].partner);
    EXPECT_EQ(BoundaryType::WakeCut, mesh.faces()[partner].boundary);
    // Same place in space.
    EXPECT_LT(distance(mesh.faceCentres()[f], mesh.faceCentres()[partner]), 1e-12);
    EXPECT_NEAR(mesh.faceAreas()[f], mesh.faceAreas()[partner], 1e-12);
    // Facing opposite ways, since one side looks up and the other down.
    EXPECT_NEAR(-1.0, dot(mesh.faceNormals()[f], mesh.faceNormals()[partner]), 1e-12);
    // They bound different cells.
    EXPECT_NE(face.owner, mesh.faces()[partner].owner);
  }
  EXPECT_EQ(mesh.countFaces(BoundaryType::WakeCut), paired);
}

TEST(CGrid, WakeCutLiesOnTheChordLineBehindTheSection) {
  const Airfoil foil = section();
  const Mesh mesh = meshFor(MeshResolution::Coarse);

  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    if (mesh.faces()[f].boundary != BoundaryType::WakeCut) {
      continue;
    }
    EXPECT_NEAR(0.0, mesh.faceCentres()[f].y, 1e-12);
    EXPECT_GE(mesh.faceCentres()[f].x, foil.trailingEdge().x - 1e-9);
  }
}

// ---------------------------------------------------------------------------
// Agreement with the analytic domain
// ---------------------------------------------------------------------------

TEST(CGrid, TotalAreaMatchesTheAnalyticDomain) {
  const CGridOptions options = optionsFor(MeshResolution::Fine);
  const Airfoil foil = section();
  const Mesh mesh = meshWith(options);

  const double expected = analyticDomainArea(options, foil);
  EXPECT_NEAR(expected, mesh.totalArea(), 1e-4 * expected);
}

// The outer boundary is a polygon inscribed in the true ellipse, so the meshed
// area is slightly short of the analytic one, by an amount that must shrink as
// the boundary is resolved with more points.
TEST(CGrid, DomainAreaConvergesUnderRefinement) {
  const Airfoil foil = section();

  double previousError = std::numeric_limits<double>::max();
  for (const MeshResolution resolution : kResolutions) {
    const CGridOptions options = optionsFor(resolution);
    const Mesh mesh = meshWith(options);

    const double error = std::abs(mesh.totalArea() - analyticDomainArea(options, foil));
    EXPECT_LT(error, previousError) << toString(resolution);
    previousError = error;
  }
}

// ---------------------------------------------------------------------------
// Refinement behaviour
// ---------------------------------------------------------------------------

TEST(CGrid, RefinementIncreasesResolutionMonotonically) {
  std::size_t previousCells = 0;
  std::size_t previousWallFaces = 0;
  double previousWallSpacing = std::numeric_limits<double>::max();
  double previousMinArea = std::numeric_limits<double>::max();

  for (const MeshResolution resolution : kResolutions) {
    const Mesh mesh = meshFor(resolution);

    EXPECT_GT(mesh.cellCount(), previousCells) << toString(resolution);
    EXPECT_GT(mesh.countFaces(BoundaryType::Wall), previousWallFaces) << toString(resolution);
    EXPECT_LT(mesh.quality().minWallSpacing, previousWallSpacing) << toString(resolution);
    EXPECT_LT(mesh.quality().minCellArea, previousMinArea) << toString(resolution);

    previousCells = mesh.cellCount();
    previousWallFaces = mesh.countFaces(BoundaryType::Wall);
    previousWallSpacing = mesh.quality().minWallSpacing;
    previousMinArea = mesh.quality().minCellArea;
  }
}

TEST(CGrid, FirstLayerHeightIsHonoured) {
  for (const double requested : {1e-3, 3e-4, 1e-4}) {
    CGridOptions options = optionsFor(MeshResolution::Medium);
    options.firstLayerHeight = requested;
    const Mesh mesh = meshWith(options);

    // Renormalising the geometric series shifts the achieved height slightly;
    // it must still land within a few percent of what was asked for.
    EXPECT_NEAR(requested, mesh.quality().minWallSpacing, 0.05 * requested)
        << "requested " << requested;
  }
}

TEST(CGrid, CellsGrowSmoothlyAwayFromTheWall) {
  const Mesh mesh = meshFor(MeshResolution::Medium);

  // Walk one line of cells out from a wall station and check consecutive
  // heights never jump abruptly. A sudden change in cell size is a classic
  // source of discretisation error.
  const int i = mesh.nodesI() / 2;  // somewhere on the section
  for (int j = 0; j + 2 < mesh.nodesJ() - 1; ++j) {
    const Vec2& a = mesh.nodes()[static_cast<std::size_t>(mesh.nodeIndex(i, j))];
    const Vec2& b = mesh.nodes()[static_cast<std::size_t>(mesh.nodeIndex(i, j + 1))];
    const Vec2& c = mesh.nodes()[static_cast<std::size_t>(mesh.nodeIndex(i, j + 2))];

    const double first = distance(a, b);
    const double second = distance(b, c);
    ASSERT_GT(first, 0.0);
    EXPECT_LT(second / first, 1.5) << "growth too fast at j=" << j;
  }
}

// ---------------------------------------------------------------------------
// Structure and options
// ---------------------------------------------------------------------------

TEST(CGrid, BlockDimensionsFollowTheOptions) {
  const CGridOptions options = optionsFor(MeshResolution::Coarse);
  const Mesh mesh = meshWith(options);

  const int wallNodes = 2 * options.surfacePoints - 1;
  EXPECT_EQ(2 * options.wakePoints + wallNodes, mesh.nodesI());
  EXPECT_EQ(options.normalPoints, mesh.nodesJ());
  EXPECT_EQ(static_cast<std::size_t>(mesh.nodesI()) * static_cast<std::size_t>(mesh.nodesJ()),
            mesh.nodeCount());
}

TEST(CGrid, ScalesWithChord) {
  const CGridOptions options = optionsFor(MeshResolution::Coarse);
  const Mesh unit = meshWith(options);

  auto scaled = generateCGrid(section("2412", 3.0), options);
  ASSERT_TRUE(scaled);

  EXPECT_EQ(unit.cellCount(), scaled.value().cellCount());
  EXPECT_NEAR(9.0 * unit.totalArea(), scaled.value().totalArea(), 1e-9 * scaled.value().totalArea());
  EXPECT_NEAR(3.0 * unit.quality().minWallSpacing, scaled.value().quality().minWallSpacing,
              1e-9);
}

TEST(CGrid, RejectsAnOpenTrailingEdge) {
  auto blunt = makeNaca4Digit("2412", AirfoilOptions{.trailingEdge = TrailingEdge::Open});
  ASSERT_TRUE(blunt);

  const auto result = generateCGrid(blunt.value(), optionsFor(MeshResolution::Coarse));
  ASSERT_TRUE(result.hasError());
  EXPECT_NE(std::string::npos, result.error().message().find("closed trailing edge"));
}

TEST(CGrid, RejectsUnusableOptions) {
  const Airfoil foil = section();

  EXPECT_TRUE(generateCGrid(foil, CGridOptions{.surfacePoints = 4}).hasError());
  EXPECT_TRUE(generateCGrid(foil, CGridOptions{.wakePoints = 1}).hasError());
  EXPECT_TRUE(generateCGrid(foil, CGridOptions{.normalPoints = 2}).hasError());
  EXPECT_TRUE(generateCGrid(foil, CGridOptions{.upstreamChords = 0.1}).hasError());
  EXPECT_TRUE(generateCGrid(foil, CGridOptions{.downstreamChords = 0.0}).hasError());
  EXPECT_TRUE(generateCGrid(foil, CGridOptions{.verticalChords = -1.0}).hasError());
  EXPECT_TRUE(generateCGrid(foil, CGridOptions{.firstLayerHeight = 0.0}).hasError());
  EXPECT_TRUE(generateCGrid(foil, CGridOptions{.firstLayerHeight = 0.5}).hasError());
}

TEST(CGrid, ResolutionsHaveDistinctNames) {
  EXPECT_EQ("Coarse", toString(MeshResolution::Coarse));
  EXPECT_EQ("Medium", toString(MeshResolution::Medium));
  EXPECT_EQ("Fine", toString(MeshResolution::Fine));
}

}  // namespace
