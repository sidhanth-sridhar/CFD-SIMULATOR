// Tests for the face-based mesh container itself, on grids simple enough that
// every metric can be written down by hand.

#include "cfd/mesh/Mesh.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <map>
#include <vector>

namespace {

using cfd::Vec2;
using cfd::mesh::BoundaryType;
using cfd::mesh::buildStructured;
using cfd::mesh::Face;
using cfd::mesh::Mesh;
using cfd::mesh::StructuredMeshSpec;

/// A uniform ni x nj block of unit squares with its corner at the origin.
StructuredMeshSpec uniformBlock(int ni, int nj, double spacing = 1.0) {
  StructuredMeshSpec spec;
  spec.nodesI = ni;
  spec.nodesJ = nj;
  spec.nodes.reserve(static_cast<std::size_t>(ni) * static_cast<std::size_t>(nj));
  for (int j = 0; j < nj; ++j) {
    for (int i = 0; i < ni; ++i) {
      spec.nodes.push_back(Vec2{spacing * i, spacing * j});
    }
  }
  spec.jMinBoundary.assign(static_cast<std::size_t>(ni - 1), BoundaryType::Wall);
  return spec;
}

Mesh build(StructuredMeshSpec spec) {
  auto result = buildStructured(std::move(spec));
  EXPECT_TRUE(result) << (result.hasError() ? result.error().format() : "");
  return std::move(result).value();
}

// ---------------------------------------------------------------------------
// Counts and connectivity
// ---------------------------------------------------------------------------

TEST(Mesh, CountsFollowTheBlockDimensions) {
  const Mesh mesh = build(uniformBlock(4, 3));

  EXPECT_EQ(12u, mesh.nodeCount());       // 4*3
  EXPECT_EQ(6u, mesh.cellCount());        // 3*2
  EXPECT_EQ(3u * 3u + 4u * 2u, mesh.faceCount());  // j-faces + i-faces
}

// Euler's formula for a connected planar subdivision, V - E + F = 2, counting
// the unbounded outer region as one face. It is a single scalar check that the
// node, face and cell counts describe a consistent planar mesh.
TEST(Mesh, SatisfiesEulersFormula) {
  for (const auto& [ni, nj] : std::vector<std::pair<int, int>>{{2, 2}, {4, 3}, {9, 7}, {20, 13}}) {
    const Mesh mesh = build(uniformBlock(ni, nj));

    const auto v = static_cast<long long>(mesh.nodeCount());
    const auto e = static_cast<long long>(mesh.faceCount());
    const auto f = static_cast<long long>(mesh.cellCount()) + 1;

    EXPECT_EQ(2, v - e + f) << ni << "x" << nj;
  }
}

TEST(Mesh, EveryFaceHasAValidOwnerAndConsistentNeighbour) {
  const Mesh mesh = build(uniformBlock(6, 5));
  const auto cells = static_cast<int>(mesh.cellCount());

  for (const Face& face : mesh.faces()) {
    ASSERT_GE(face.owner, 0);
    ASSERT_LT(face.owner, cells);

    if (face.isInterior()) {
      ASSERT_GE(face.neighbour, 0);
      ASSERT_LT(face.neighbour, cells);
      EXPECT_NE(face.owner, face.neighbour);
    } else {
      EXPECT_EQ(-1, face.neighbour);
    }

    EXPECT_GE(face.nodes[0], 0);
    EXPECT_GE(face.nodes[1], 0);
    EXPECT_NE(face.nodes[0], face.nodes[1]);
  }
}

// An interior face must be listed by exactly the two cells it separates; a
// boundary face by exactly one. Anything else means the connectivity does not
// describe a watertight partition of the domain.
TEST(Mesh, EachFaceIsReferencedByTheCorrectNumberOfCells) {
  const Mesh mesh = build(uniformBlock(7, 6));

  std::map<int, int> references;
  for (const auto& faces : mesh.cellFaces()) {
    for (const int f : faces) {
      ++references[f];
    }
  }

  ASSERT_EQ(mesh.faceCount(), references.size()) << "some face belongs to no cell";
  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    const int expected = mesh.faces()[f].isInterior() ? 2 : 1;
    EXPECT_EQ(expected, references[static_cast<int>(f)]) << "face " << f;
  }
}

TEST(Mesh, EachCellIsOwnerOrNeighbourOfAllFourOfItsFaces) {
  const Mesh mesh = build(uniformBlock(5, 4));

  for (std::size_t c = 0; c < mesh.cellCount(); ++c) {
    const auto& faces = mesh.cellFaces()[c];
    for (const int f : faces) {
      const Face& face = mesh.faces()[static_cast<std::size_t>(f)];
      const bool attached = face.owner == static_cast<int>(c) ||
                            face.neighbour == static_cast<int>(c);
      EXPECT_TRUE(attached) << "cell " << c << " lists face " << f;
    }
    // All four distinct.
    for (std::size_t a = 0; a < 4; ++a) {
      for (std::size_t b = a + 1; b < 4; ++b) {
        EXPECT_NE(faces[a], faces[b]) << "cell " << c;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Metrics on a grid where the answers are obvious
// ---------------------------------------------------------------------------

TEST(Mesh, UnitSquaresHaveUnitAreaAndCentredCentroids) {
  const Mesh mesh = build(uniformBlock(4, 3));

  for (std::size_t c = 0; c < mesh.cellCount(); ++c) {
    EXPECT_NEAR(1.0, mesh.cellAreas()[c], 1e-14);
  }
  // Cell (0,0) spans [0,1]x[0,1].
  const std::size_t first = static_cast<std::size_t>(mesh.cellIndex(0, 0));
  EXPECT_NEAR(0.5, mesh.cellCentroids()[first].x, 1e-14);
  EXPECT_NEAR(0.5, mesh.cellCentroids()[first].y, 1e-14);

  EXPECT_NEAR(6.0, mesh.totalArea(), 1e-13);
}

TEST(Mesh, FaceAreasAreEdgeLengths) {
  const Mesh mesh = build(uniformBlock(4, 3, 2.0));

  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    EXPECT_NEAR(2.0, mesh.faceAreas()[f], 1e-14);
    EXPECT_NEAR(1.0, length(mesh.faceNormals()[f]), 1e-14) << "normal must be a unit vector";
  }
}

// The normal convention: always out of the owner. For an interior face that
// means it points from the owner's centroid towards the neighbour's.
TEST(Mesh, InteriorNormalsPointFromOwnerToNeighbour) {
  const Mesh mesh = build(uniformBlock(6, 5));

  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    const Face& face = mesh.faces()[f];
    if (!face.isInterior()) {
      continue;
    }
    const Vec2 link = mesh.cellCentroids()[static_cast<std::size_t>(face.neighbour)] -
                      mesh.cellCentroids()[static_cast<std::size_t>(face.owner)];
    EXPECT_GT(dot(mesh.faceNormals()[f], link), 0.0) << "face " << f;
  }
}

TEST(Mesh, BoundaryNormalsPointOutOfTheDomain) {
  const Mesh mesh = build(uniformBlock(6, 5));

  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    const Face& face = mesh.faces()[f];
    if (face.isInterior()) {
      continue;
    }
    const Vec2 outward =
        mesh.faceCentres()[f] - mesh.cellCentroids()[static_cast<std::size_t>(face.owner)];
    EXPECT_GT(dot(mesh.faceNormals()[f], outward), 0.0) << "face " << f;
  }
}

// The single most important identity a finite-volume mesh must satisfy. For
// any closed cell, the outward area vectors sum to zero:
//
//     sum_faces ( n * A ) = 0
//
// It is what makes a constant field have zero divergence discretely, so a
// solver built on a mesh that fails this cannot even preserve a uniform flow.
TEST(Mesh, AreaVectorsCloseAroundEveryCell) {
  const Mesh mesh = build(uniformBlock(8, 6, 0.35));

  for (std::size_t c = 0; c < mesh.cellCount(); ++c) {
    Vec2 sum{0.0, 0.0};
    double scale = 0.0;
    for (const int f : mesh.cellFaces()[c]) {
      const auto face = static_cast<std::size_t>(f);
      const double sign = (mesh.faces()[face].owner == static_cast<int>(c)) ? 1.0 : -1.0;
      sum += mesh.faceNormals()[face] * (mesh.faceAreas()[face] * sign);
      scale += mesh.faceAreas()[face];
    }
    EXPECT_LT(length(sum), 1e-12 * scale) << "cell " << c;
  }
}

// Cell area recovered from its faces by the divergence theorem, applied to the
// field F = (x, y) whose divergence is 2:
//
//     A = 1/2 * sum_faces ( centre . n ) * A_face
//
// Cross-checks centroids, normals and face areas against the shoelace areas.
TEST(Mesh, CellAreasAgreeWithTheDivergenceTheorem) {
  const Mesh mesh = build(uniformBlock(7, 5, 1.7));

  for (std::size_t c = 0; c < mesh.cellCount(); ++c) {
    double flux = 0.0;
    for (const int f : mesh.cellFaces()[c]) {
      const auto face = static_cast<std::size_t>(f);
      const double sign = (mesh.faces()[face].owner == static_cast<int>(c)) ? 1.0 : -1.0;
      flux += dot(mesh.faceCentres()[face], mesh.faceNormals()[face]) *
              mesh.faceAreas()[face] * sign;
    }
    EXPECT_NEAR(mesh.cellAreas()[c], 0.5 * flux, 1e-12 * mesh.cellAreas()[c]) << "cell " << c;
  }
}

TEST(Mesh, ReportsQualityOfAWellFormedGrid) {
  const Mesh mesh = build(uniformBlock(6, 6, 0.5));

  EXPECT_EQ(0u, mesh.quality().invertedCells);
  EXPECT_NEAR(0.25, mesh.quality().minCellArea, 1e-14);
  EXPECT_NEAR(0.25, mesh.quality().maxCellArea, 1e-14);
  EXPECT_NEAR(1.0, mesh.quality().maxAspectRatio, 1e-12);
  // A Cartesian grid is perfectly orthogonal.
  EXPECT_NEAR(0.0, mesh.quality().maxNonOrthogonalityDeg, 1e-9);
  EXPECT_NEAR(0.5, mesh.quality().minWallSpacing, 1e-14);
}

// A deliberately folded block: the container must report the inversion rather
// than silently accept it.
TEST(Mesh, DetectsInvertedCells) {
  StructuredMeshSpec spec = uniformBlock(3, 3);
  // Drag one interior node far past its neighbours so two cells fold over.
  spec.nodes[static_cast<std::size_t>(1 * 3 + 1)] = Vec2{-5.0, -5.0};

  const Mesh mesh = build(std::move(spec));
  EXPECT_GT(mesh.quality().invertedCells, 0u);
}

// ---------------------------------------------------------------------------
// Boundaries
// ---------------------------------------------------------------------------

TEST(Mesh, BoundaryFacesAreTaggedAndCounted) {
  const int ni = 6;
  const int nj = 4;
  const Mesh mesh = build(uniformBlock(ni, nj));

  EXPECT_EQ(static_cast<std::size_t>(ni - 1), mesh.countFaces(BoundaryType::Wall));
  EXPECT_EQ(static_cast<std::size_t>(ni - 1), mesh.countFaces(BoundaryType::Farfield));
  EXPECT_EQ(static_cast<std::size_t>(2 * (nj - 1)), mesh.countFaces(BoundaryType::Outlet));

  const std::size_t boundary = mesh.countFaces(BoundaryType::Wall) +
                               mesh.countFaces(BoundaryType::Farfield) +
                               mesh.countFaces(BoundaryType::Outlet);
  EXPECT_EQ(mesh.faceCount() - mesh.countFaces(BoundaryType::Interior), boundary);

  // The boundary of a rectangular block is its perimeter.
  EXPECT_EQ(static_cast<std::size_t>(2 * (ni - 1) + 2 * (nj - 1)), boundary);
}

TEST(Mesh, RejectsMalformedSpecifications) {
  StructuredMeshSpec tooSmall = uniformBlock(1, 4);
  EXPECT_TRUE(buildStructured(std::move(tooSmall)).hasError());

  StructuredMeshSpec wrongNodes = uniformBlock(4, 4);
  wrongNodes.nodes.pop_back();
  EXPECT_TRUE(buildStructured(std::move(wrongNodes)).hasError());

  StructuredMeshSpec wrongBoundary = uniformBlock(4, 4);
  wrongBoundary.jMinBoundary.pop_back();
  EXPECT_TRUE(buildStructured(std::move(wrongBoundary)).hasError());
}

TEST(Mesh, StructuredIndexingMatchesNodeOrder) {
  const int ni = 5;
  const int nj = 4;
  const Mesh mesh = build(uniformBlock(ni, nj, 3.0));

  EXPECT_TRUE(mesh.isStructured());
  EXPECT_EQ(ni, mesh.nodesI());
  EXPECT_EQ(nj, mesh.nodesJ());

  for (int j = 0; j < nj; ++j) {
    for (int i = 0; i < ni; ++i) {
      const Vec2& node = mesh.nodes()[static_cast<std::size_t>(mesh.nodeIndex(i, j))];
      EXPECT_NEAR(3.0 * i, node.x, 1e-14);
      EXPECT_NEAR(3.0 * j, node.y, 1e-14);
    }
  }
}

}  // namespace
