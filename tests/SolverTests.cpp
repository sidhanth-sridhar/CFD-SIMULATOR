// Tests for the numerical building blocks: node distributions, the box mesh,
// gradient reconstruction and the linear solvers.

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "cfd/mesh/BoxGrid.hpp"
#include "cfd/mesh/Distribution.hpp"
#include "cfd/solver/Gradient.hpp"
#include "cfd/solver/LinearSystem.hpp"

namespace {

using cfd::Vec2;
using cfd::mesh::BoundaryType;
using cfd::mesh::BoxOptions;
using cfd::mesh::generateBox;
using cfd::mesh::Grading;
using cfd::mesh::Mesh;
using cfd::solver::conjugateGradient;
using cfd::solver::gaussSeidel;
using cfd::solver::greenGaussGradient;
using cfd::solver::LinearSystem;

Mesh box(const BoxOptions& options) {
  auto result = generateBox(options);
  EXPECT_TRUE(result) << (result.hasError() ? result.error().format() : "");
  return std::move(result).value();
}

// ---------------------------------------------------------------------------
// Node distributions
// ---------------------------------------------------------------------------

TEST(Distribution, UniformIsEvenlySpaced) {
  const std::vector<double> t = cfd::mesh::uniformDistribution(4);
  ASSERT_EQ(5u, t.size());
  for (std::size_t i = 0; i < t.size(); ++i) {
    EXPECT_NEAR(static_cast<double>(i) / 4.0, t[i], 1e-15);
  }
}

TEST(Distribution, GeometricStartsAtTheRequestedSpacingAndSpansTheUnit) {
  const std::vector<double> t = cfd::mesh::geometricDistribution(0.001, 1.0, 40);

  ASSERT_EQ(41u, t.size());
  EXPECT_DOUBLE_EQ(0.0, t.front());
  EXPECT_DOUBLE_EQ(1.0, t.back());
  EXPECT_NEAR(0.001, t[1] - t[0], 1e-6);
  for (std::size_t i = 0; i + 1 < t.size(); ++i) {
    EXPECT_LT(t[i], t[i + 1]) << "at " << i;
  }
  // Each interval is larger than the last.
  for (std::size_t i = 1; i + 1 < t.size(); ++i) {
    EXPECT_GE(t[i + 1] - t[i], t[i] - t[i - 1] - 1e-15) << "at " << i;
  }
}

TEST(Distribution, SymmetricIsExactlyMirrored) {
  const std::vector<double> t = cfd::mesh::symmetricGeometricDistribution(0.002, 1.0, 30);

  ASSERT_EQ(31u, t.size());
  for (std::size_t i = 0; i < t.size(); ++i) {
    EXPECT_NEAR(1.0 - t[t.size() - 1 - i], t[i], 1e-14) << "at " << i;
  }
  EXPECT_NEAR(0.002, t[1] - t[0], 1e-6);
}

TEST(Distribution, RatioSolverReproducesTheRequestedSum) {
  const double ratio = cfd::mesh::solveGeometricRatio(0.001, 1.0, 50);
  EXPECT_GT(ratio, 1.0);

  double sum = 0.0;
  double step = 0.001;
  for (int k = 0; k < 50; ++k) {
    sum += step;
    step *= ratio;
  }
  EXPECT_NEAR(1.0, sum, 1e-9);
}

// ---------------------------------------------------------------------------
// Box mesh
// ---------------------------------------------------------------------------

TEST(BoxGrid, HasTheRequestedSizeAndPositiveCells) {
  const Mesh mesh = box(BoxOptions{.length = 2.0, .height = 0.5, .cellsX = 8, .cellsY = 4});

  EXPECT_EQ(9, mesh.nodesI());
  EXPECT_EQ(5, mesh.nodesJ());
  EXPECT_EQ(32u, mesh.cellCount());
  EXPECT_EQ(0u, mesh.quality().invertedCells);
  EXPECT_NEAR(1.0, mesh.totalArea(), 1e-12);  // 2.0 * 0.5
  // A Cartesian mesh is perfectly orthogonal.
  EXPECT_NEAR(0.0, mesh.quality().maxNonOrthogonalityDeg, 1e-9);
}

TEST(BoxGrid, SidesCarryTheRequestedBoundaryTypes) {
  const Mesh mesh = box(BoxOptions{.cellsX = 6,
                                   .cellsY = 3,
                                   .left = BoundaryType::Farfield,
                                   .right = BoundaryType::Outlet,
                                   .lower = BoundaryType::Wall,
                                   .upper = BoundaryType::Wall});

  EXPECT_EQ(3u, mesh.countFaces(BoundaryType::Farfield));  // left, one per cell in y
  EXPECT_EQ(3u, mesh.countFaces(BoundaryType::Outlet));
  EXPECT_EQ(12u, mesh.countFaces(BoundaryType::Wall));  // top and bottom

  // The left faces really are on the left.
  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    if (mesh.faces()[f].boundary == BoundaryType::Farfield) {
      EXPECT_NEAR(0.0, mesh.faceCentres()[f].x, 1e-12);
    }
  }
}

TEST(BoxGrid, GradingClustersCellsAtTheWall) {
  const Mesh mesh = box(BoxOptions{.height = 1.0,
                                   .cellsX = 4,
                                   .cellsY = 20,
                                   .grading = Grading::TowardLower,
                                   .firstCellHeight = 0.002});

  const double firstCell = mesh.nodes()[static_cast<std::size_t>(mesh.nodeIndex(0, 1))].y -
                           mesh.nodes()[static_cast<std::size_t>(mesh.nodeIndex(0, 0))].y;
  EXPECT_NEAR(0.002, firstCell, 1e-5);
  EXPECT_GT(mesh.quality().maxAspectRatio, 10.0) << "graded cells should be thin";
}

TEST(BoxGrid, RejectsDegenerateOptions) {
  EXPECT_TRUE(generateBox(BoxOptions{.cellsX = 0}).hasError());
  EXPECT_TRUE(generateBox(BoxOptions{.length = 0.0}).hasError());
  EXPECT_TRUE(generateBox(BoxOptions{.height = -1.0}).hasError());
  EXPECT_TRUE(generateBox(BoxOptions{.grading = Grading::TowardLower,
                                     .firstCellHeight = 0.9})
                  .hasError());
}

// ---------------------------------------------------------------------------
// Green-Gauss gradient
// ---------------------------------------------------------------------------

/// Face values sampled from an analytic field, so the reconstruction is being
/// tested rather than the interpolation feeding it.
std::vector<double> sampleFaces(const Mesh& mesh, double (*fn)(const Vec2&)) {
  std::vector<double> values(mesh.faceCount(), 0.0);
  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    values[f] = fn(mesh.faceCentres()[f]);
  }
  return values;
}

// Green-Gauss is exact for a linear field: the face average of a linear
// function is its value at the face centre, so the divergence theorem closes
// exactly. This must hold on a graded mesh too.
TEST(Gradient, IsExactForALinearField) {
  const Mesh mesh = box(BoxOptions{.length = 2.0,
                                   .height = 1.0,
                                   .cellsX = 10,
                                   .cellsY = 12,
                                   .grading = Grading::TowardLower,
                                   .firstCellHeight = 0.01});

  // phi = 3x - 2y + 1, so grad phi = (3, -2) everywhere.
  auto faces = sampleFaces(mesh, [](const Vec2& p) { return 3.0 * p.x - 2.0 * p.y + 1.0; });
  auto result = greenGaussGradient(mesh, faces);
  ASSERT_TRUE(result);

  for (std::size_t c = 0; c < mesh.cellCount(); ++c) {
    EXPECT_NEAR(3.0, result.value()[c].x, 1e-10) << "cell " << c;
    EXPECT_NEAR(-2.0, result.value()[c].y, 1e-10) << "cell " << c;
  }
}

// A quadratic would not exercise this: on a Cartesian mesh the face-centre
// value of x^2 is also its average over the face, so Green-Gauss happens to be
// exact and the test would compare two zeros. A trigonometric field has a
// genuine truncation error, which must shrink as the mesh is refined.
TEST(Gradient, ConvergesForANonPolynomialField) {
  const auto errorFor = [](int cells) {
    const Mesh mesh = box(BoxOptions{.length = 1.0, .height = 1.0,
                                     .cellsX = cells, .cellsY = cells});
    auto faces = sampleFaces(
        mesh, [](const Vec2& p) { return std::sin(2.0 * p.x) * std::cos(3.0 * p.y); });
    auto result = greenGaussGradient(mesh, faces);
    EXPECT_TRUE(result);

    double worst = 0.0;
    for (std::size_t c = 0; c < mesh.cellCount(); ++c) {
      const Vec2& p = mesh.cellCentroids()[c];
      const double dx = 2.0 * std::cos(2.0 * p.x) * std::cos(3.0 * p.y);
      const double dy = -3.0 * std::sin(2.0 * p.x) * std::sin(3.0 * p.y);
      worst = std::max(worst, std::abs(result.value()[c].x - dx));
      worst = std::max(worst, std::abs(result.value()[c].y - dy));
    }
    return worst;
  };

  const double coarse = errorFor(10);
  const double fine = errorFor(20);
  const double finer = errorFor(40);

  EXPECT_GT(coarse, 0.0);
  EXPECT_LT(fine, coarse);
  EXPECT_LT(finer, fine);
  // Second order: halving the spacing should cut the error by about four.
  EXPECT_LT(finer, coarse / 8.0);
}

TEST(Gradient, RejectsMismatchedFaceValues) {
  const Mesh mesh = box(BoxOptions{.cellsX = 4, .cellsY = 4});
  EXPECT_TRUE(greenGaussGradient(mesh, std::vector<double>(3, 0.0)).hasError());
}

// ---------------------------------------------------------------------------
// Linear solvers
// ---------------------------------------------------------------------------

/// A symmetric, diagonally dominant system on a small mesh: a discrete
/// Laplacian with a unit diagonal boost so it is non-singular.
LinearSystem laplacian(const Mesh& mesh, const std::vector<double>& rhs) {
  LinearSystem system(mesh.cellCount(), mesh.faceCount());
  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    const auto& face = mesh.faces()[f];
    if (face.neighbour < 0) {
      continue;
    }
    const double coefficient = 1.0;
    system.diagonal[static_cast<std::size_t>(face.owner)] += coefficient;
    system.diagonal[static_cast<std::size_t>(face.neighbour)] += coefficient;
    system.upper[f] = -coefficient;
    system.lower[f] = -coefficient;
  }
  for (std::size_t c = 0; c < mesh.cellCount(); ++c) {
    system.diagonal[c] += 1.0;
    system.source[c] = rhs[c];
  }
  return system;
}

TEST(LinearSystem, MultiplyMatchesAHandBuiltProduct) {
  const Mesh mesh = box(BoxOptions{.cellsX = 3, .cellsY = 2});
  std::vector<double> rhs(mesh.cellCount(), 1.0);
  const LinearSystem system = laplacian(mesh, rhs);

  std::vector<double> x(mesh.cellCount());
  for (std::size_t c = 0; c < x.size(); ++c) {
    x[c] = static_cast<double>(c + 1);
  }

  std::vector<double> y;
  system.multiply(mesh, x, y);

  // Recompute cell 0 by hand from its faces.
  double expected = system.diagonal[0] * x[0];
  for (const int faceIndex : mesh.cellFaces()[0]) {
    const auto f = static_cast<std::size_t>(faceIndex);
    const auto& face = mesh.faces()[f];
    if (face.neighbour < 0) {
      continue;
    }
    if (face.owner == 0) {
      expected += system.upper[f] * x[static_cast<std::size_t>(face.neighbour)];
    } else {
      expected += system.lower[f] * x[static_cast<std::size_t>(face.owner)];
    }
  }
  EXPECT_NEAR(expected, y[0], 1e-12);
}

TEST(LinearSystem, DetectsSymmetry) {
  const Mesh mesh = box(BoxOptions{.cellsX = 3, .cellsY = 3});
  std::vector<double> rhs(mesh.cellCount(), 1.0);
  LinearSystem system = laplacian(mesh, rhs);

  EXPECT_TRUE(system.isSymmetric(mesh));

  // Perturb an *interior* face: boundary faces carry no off-diagonal pair, so
  // asymmetry there is not meaningful and is deliberately not reported.
  std::size_t interior = 0;
  while (interior < mesh.faceCount() && !mesh.faces()[interior].isInterior()) {
    ++interior;
  }
  ASSERT_LT(interior, mesh.faceCount());
  system.upper[interior] += 1.0;
  EXPECT_FALSE(system.isSymmetric(mesh));
}

TEST(GaussSeidel, DrivesTheResidualDown) {
  const Mesh mesh = box(BoxOptions{.cellsX = 8, .cellsY = 8});
  std::vector<double> rhs(mesh.cellCount(), 1.0);
  const LinearSystem system = laplacian(mesh, rhs);

  std::vector<double> x(mesh.cellCount(), 0.0);
  const double before = system.residualL1(mesh, x);
  gaussSeidel(mesh, system, x, 200);
  const double after = system.residualL1(mesh, x);

  EXPECT_LT(after, 1e-8 * before);
}

TEST(ConjugateGradient, SolvesASymmetricSystem) {
  const Mesh mesh = box(BoxOptions{.cellsX = 12, .cellsY = 12});

  // Build a right-hand side from a known answer, then check it is recovered.
  std::vector<double> expected(mesh.cellCount());
  for (std::size_t c = 0; c < expected.size(); ++c) {
    const Vec2& centroid = mesh.cellCentroids()[c];
    expected[c] = std::sin(3.0 * centroid.x) * std::cos(2.0 * centroid.y);
  }
  LinearSystem system = laplacian(mesh, std::vector<double>(mesh.cellCount(), 0.0));
  system.multiply(mesh, expected, system.source);

  std::vector<double> x(mesh.cellCount(), 0.0);
  const auto report = conjugateGradient(mesh, system, x, 1e-12, 500);

  EXPECT_TRUE(report.converged);
  EXPECT_GT(report.iterations, 0);
  for (std::size_t c = 0; c < x.size(); ++c) {
    EXPECT_NEAR(expected[c], x[c], 1e-8) << "cell " << c;
  }
}

TEST(ConjugateGradient, ReportsAnAlreadySolvedSystem) {
  const Mesh mesh = box(BoxOptions{.cellsX = 4, .cellsY = 4});
  LinearSystem system = laplacian(mesh, std::vector<double>(mesh.cellCount(), 0.0));

  std::vector<double> x(mesh.cellCount(), 0.0);
  const auto report = conjugateGradient(mesh, system, x, 1e-10, 100);
  EXPECT_TRUE(report.converged);
  EXPECT_EQ(0, report.iterations);
}

}  // namespace
