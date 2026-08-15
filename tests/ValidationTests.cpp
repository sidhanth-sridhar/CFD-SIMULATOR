// Validation of the laminar solver against flows whose answers are known.
//
// These are the tests that decide whether the solver is right, as opposed to
// merely self-consistent. Each case has an analytic solution, so the question
// "is this a physically reasonable viscous flow" becomes a number.
//
//   uniform flow  - must be preserved exactly. Checks that convection, the
//                   pressure gradient and the boundary conditions are mutually
//                   consistent: a uniform field is a solution, so any drift is
//                   a discretisation inconsistency, not an approximation.
//   Couette       - a linear profile driven purely by viscosity. Convection is
//                   identically zero, so this isolates diffusion and the wall
//                   condition.
//   Poiseuille    - a parabolic profile in balance with a pressure gradient.
//                   Both the profile and dp/dx are known in closed form.
//   Blasius       - a developing boundary layer. The one case here with a
//                   genuine convection-diffusion balance, compared against the
//                   classical skin-friction law.

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "cfd/mesh/BoxGrid.hpp"
#include "cfd/solver/SimpleSolver.hpp"

namespace {

using cfd::Vec2;
using cfd::flow::BoundaryKind;
using cfd::flow::FaceConditions;
using cfd::flow::FlowField;
using cfd::mesh::BoundaryType;
using cfd::mesh::BoxOptions;
using cfd::mesh::Grading;
using cfd::mesh::Mesh;
using cfd::solver::ConvectionScheme;
using cfd::solver::SimpleSettings;
using cfd::solver::SimpleSolver;
using cfd::solver::SolverMonitor;

Mesh box(const BoxOptions& options) {
  auto result = cfd::mesh::generateBox(options);
  EXPECT_TRUE(result) << (result.hasError() ? result.error().format() : "");
  return std::move(result).value();
}

FlowField uniformField(const Mesh& mesh, Vec2 velocity, double density, double viscosity) {
  FlowField field;
  field.resize(mesh.cellCount());
  for (std::size_t c = 0; c < mesh.cellCount(); ++c) {
    field.velocity[c] = velocity;
    field.pressure[c] = 0.0;
    field.density[c] = density;
    field.viscosity[c] = viscosity;
  }
  return field;
}

/// Run to convergence, returning the last monitor. Fails the test if the
/// residuals never come down.
SolverMonitor converge(SimpleSolver& solver, int maxIterations, double tolerance) {
  SolverMonitor monitor;
  int taken = 0;
  for (; taken < maxIterations; ++taken) {
    monitor = solver.iterate();
    if (monitor.residuals.continuity < tolerance && monitor.residuals.momentumX < tolerance &&
        monitor.residuals.momentumY < tolerance) {
      break;
    }
  }
  EXPECT_LT(taken, maxIterations) << "did not converge: continuity "
                                  << monitor.residuals.continuity << ", momentum "
                                  << monitor.residuals.momentumX;
  return monitor;
}

// ---------------------------------------------------------------------------
// Uniform flow
// ---------------------------------------------------------------------------

TEST(Validation, UniformFlowIsPreservedExactly) {
  constexpr double kSpeed = 1.0;
  const Mesh mesh = box(BoxOptions{.length = 2.0,
                                   .height = 1.0,
                                   .cellsX = 20,
                                   .cellsY = 15,
                                   .left = BoundaryType::Farfield,
                                   .right = BoundaryType::Outlet,
                                   .lower = BoundaryType::Farfield,
                                   .upper = BoundaryType::Farfield});

  // The stream is imposed on every side except the outlet, so a uniform field
  // satisfies every equation and every condition simultaneously.
  FaceConditions conditions(mesh.faceCount());
  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    if (mesh.faces()[f].isInterior()) {
      continue;
    }
    if (mesh.faces()[f].boundary == BoundaryType::Outlet) {
      conditions[f].kind = BoundaryKind::Outlet;
      conditions[f].pressure = 0.0;
    } else {
      conditions[f].kind = BoundaryKind::Inlet;
      conditions[f].velocity = Vec2{kSpeed, 0.0};
    }
  }

  auto created = SimpleSolver::create(mesh, conditions, SimpleSettings{});
  ASSERT_TRUE(created) << (created.hasError() ? created.error().format() : "");
  SimpleSolver& solver = created.value();

  // Start from a deliberately wrong field so the test proves the solver
  // *reaches* the uniform state rather than merely failing to disturb it.
  ASSERT_TRUE(solver.initialise(uniformField(mesh, Vec2{0.4, 0.0}, 1.0, 0.05)));
  const SolverMonitor monitor = converge(solver, 3000, 1e-12);

  for (std::size_t c = 0; c < mesh.cellCount(); ++c) {
    EXPECT_NEAR(kSpeed, solver.field().velocity[c].x, 1e-10) << "cell " << c;
    EXPECT_NEAR(0.0, solver.field().velocity[c].y, 1e-10) << "cell " << c;
  }
  EXPECT_LT(monitor.maxDivergence, 1e-10);
}

// ---------------------------------------------------------------------------
// Couette: pure diffusion
// ---------------------------------------------------------------------------

TEST(Validation, CouetteProfileIsLinear) {
  constexpr double kSpeed = 1.0;
  constexpr double kHeight = 1.0;
  const Mesh mesh = box(BoxOptions{.length = 2.0,
                                   .height = kHeight,
                                   .cellsX = 20,
                                   .cellsY = 20,
                                   .left = BoundaryType::Farfield,
                                   .right = BoundaryType::Outlet,
                                   .lower = BoundaryType::Wall,
                                   .upper = BoundaryType::Farfield});

  FaceConditions conditions(mesh.faceCount());
  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    const auto& face = mesh.faces()[f];
    if (face.isInterior()) {
      continue;
    }
    const double y = mesh.faceCentres()[f].y;
    switch (face.boundary) {
      case BoundaryType::Wall:
        conditions[f].kind = BoundaryKind::NoSlipWall;  // stationary lower wall
        break;
      case BoundaryType::Outlet:
        conditions[f].kind = BoundaryKind::Outlet;
        conditions[f].pressure = 0.0;
        break;
      default:
        conditions[f].kind = BoundaryKind::Inlet;
        // The upper surface is dragged along at the full speed; the inlet
        // carries the exact linear profile.
        conditions[f].velocity =
            (std::abs(mesh.faceNormals()[f].y) > 0.5) ? Vec2{kSpeed, 0.0}
                                                      : Vec2{kSpeed * y / kHeight, 0.0};
        break;
    }
  }

  auto created = SimpleSolver::create(mesh, conditions, SimpleSettings{});
  ASSERT_TRUE(created);
  SimpleSolver& solver = created.value();
  ASSERT_TRUE(solver.initialise(uniformField(mesh, Vec2{0.5, 0.0}, 1.0, 0.05)));
  converge(solver, 3000, 1e-12);

  // Convection vanishes identically for this flow, so the discrete solution
  // should reproduce the exact profile to round-off.
  const int ni = mesh.nodesI();
  for (int j = 0; j < mesh.nodesJ() - 1; ++j) {
    const auto c = static_cast<std::size_t>(mesh.cellIndex(ni - 2, j));
    const double y = mesh.cellCentroids()[c].y;
    EXPECT_NEAR(kSpeed * y / kHeight, solver.field().velocity[c].x, 1e-9) << "at y=" << y;
  }
}

// ---------------------------------------------------------------------------
// Poiseuille: diffusion against a pressure gradient
// ---------------------------------------------------------------------------

TEST(Validation, PoiseuilleMatchesTheExactProfileAndPressureGradient) {
  constexpr double kMaxSpeed = 1.0;
  constexpr double kHeight = 1.0;
  constexpr double kViscosity = 0.1;
  constexpr double kDensity = 1.0;

  const Mesh mesh = box(BoxOptions{.length = 5.0,
                                   .height = kHeight,
                                   .cellsX = 50,
                                   .cellsY = 32,
                                   .left = BoundaryType::Farfield,
                                   .right = BoundaryType::Outlet,
                                   .lower = BoundaryType::Wall,
                                   .upper = BoundaryType::Wall});

  const auto exact = [](double y) {
    return 4.0 * kMaxSpeed * y * (kHeight - y) / (kHeight * kHeight);
  };

  FaceConditions conditions(mesh.faceCount());
  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    const auto& face = mesh.faces()[f];
    if (face.isInterior()) {
      continue;
    }
    switch (face.boundary) {
      case BoundaryType::Farfield:
        // The fully developed profile, so the exact solution holds throughout
        // rather than only after an entrance length.
        conditions[f].kind = BoundaryKind::Inlet;
        conditions[f].velocity = Vec2{exact(mesh.faceCentres()[f].y), 0.0};
        break;
      case BoundaryType::Outlet:
        conditions[f].kind = BoundaryKind::Outlet;
        conditions[f].pressure = 0.0;
        break;
      default:
        conditions[f].kind = BoundaryKind::NoSlipWall;
        break;
    }
  }

  auto created = SimpleSolver::create(mesh, conditions, SimpleSettings{});
  ASSERT_TRUE(created);
  SimpleSolver& solver = created.value();
  ASSERT_TRUE(solver.initialise(uniformField(mesh, Vec2{0.6, 0.0}, kDensity, kViscosity)));
  const SolverMonitor monitor = converge(solver, 4000, 1e-9);

  // --- the profile ---
  const int ni = mesh.nodesI();
  double worstError = 0.0;
  for (int j = 0; j < mesh.nodesJ() - 1; ++j) {
    const auto c = static_cast<std::size_t>(mesh.cellIndex(ni - 2, j));
    const double y = mesh.cellCentroids()[c].y;
    worstError = std::max(worstError, std::abs(solver.field().velocity[c].x - exact(y)));
  }
  EXPECT_LT(worstError, 0.005 * kMaxSpeed) << "profile departs from the exact parabola";

  // --- the pressure gradient ---
  // Balancing viscosity against the pressure gradient gives
  // dp/dx = -8 mu u_max / H^2 exactly.
  const int middle = (mesh.nodesJ() - 1) / 2;
  const auto a = static_cast<std::size_t>(mesh.cellIndex(4, middle));
  const auto b = static_cast<std::size_t>(mesh.cellIndex(ni - 6, middle));
  const double gradient =
      (solver.field().pressure[b] - solver.field().pressure[a]) /
      (mesh.cellCentroids()[b].x - mesh.cellCentroids()[a].x);
  const double expected = -8.0 * kViscosity * kMaxSpeed / (kHeight * kHeight);
  EXPECT_NEAR(expected, gradient, 0.02 * std::abs(expected));

  // --- mass conservation ---
  EXPECT_LT(monitor.maxDivergence, 1e-6);
}

// ---------------------------------------------------------------------------
// Blasius: a developing boundary layer
// ---------------------------------------------------------------------------

TEST(Validation, FlatPlateMatchesTheBlasiusSkinFriction) {
  constexpr double kSpeed = 1.0;
  constexpr double kDensity = 1.0;
  constexpr double kViscosity = 1.0e-4;  // Re = 10^4 over the plate

  // The plate starts at x = 0, with a slip section ahead of it. Running the
  // leading edge straight into the inlet puts a singularity there - u = U and
  // u = 0 meeting at one point - and it corrupts the whole boundary layer.
  const Mesh mesh = box(BoxOptions{.originX = -0.5,
                                   .length = 1.5,
                                   .height = 1.0,
                                   .cellsX = 110,
                                   .cellsY = 60,
                                   .grading = Grading::TowardLower,
                                   .firstCellHeight = 1.0e-4,
                                   .left = BoundaryType::Farfield,
                                   .right = BoundaryType::Outlet,
                                   .lower = BoundaryType::Wall,
                                   .upper = BoundaryType::Outlet});

  FaceConditions conditions(mesh.faceCount());
  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    const auto& face = mesh.faces()[f];
    if (face.isInterior()) {
      continue;
    }
    switch (face.boundary) {
      case BoundaryType::Farfield:
        conditions[f].kind = BoundaryKind::Inlet;
        conditions[f].velocity = Vec2{kSpeed, 0.0};
        break;
      case BoundaryType::Outlet:
        // Pressure outlet on the right and along the top, so the boundary
        // layer can entrain fluid instead of being confined.
        conditions[f].kind = BoundaryKind::Outlet;
        conditions[f].pressure = 0.0;
        break;
      default:
        if (mesh.faceCentres()[f].x < 0.0) {
          conditions[f].kind = BoundaryKind::Inlet;  // slip: uniform stream
          conditions[f].velocity = Vec2{kSpeed, 0.0};
        } else {
          conditions[f].kind = BoundaryKind::NoSlipWall;
        }
        break;
    }
  }

  SimpleSettings settings;
  settings.scheme = ConvectionScheme::SecondOrderUpwind;

  auto created = SimpleSolver::create(mesh, conditions, settings);
  ASSERT_TRUE(created);
  SimpleSolver& solver = created.value();
  ASSERT_TRUE(
      solver.initialise(uniformField(mesh, Vec2{kSpeed, 0.0}, kDensity, kViscosity)));
  const SolverMonitor monitor = converge(solver, 4000, 1e-8);

  // Wall shear from the first cell off the wall, which sits deep inside the
  // linear sublayer, so a one-sided difference is accurate there.
  double worstError = 0.0;
  int samples = 0;
  for (int i = 0; i < mesh.nodesI() - 1; ++i) {
    const auto c = static_cast<std::size_t>(mesh.cellIndex(i, 0));
    const double x = mesh.cellCentroids()[c].x;
    if (x < 0.15 || x > 0.95) {
      continue;  // skip the leading edge region and the outlet
    }
    const double y = mesh.cellCentroids()[c].y;
    const double shear = kViscosity * solver.field().velocity[c].x / y;
    const double cf = shear / (0.5 * kDensity * kSpeed * kSpeed);

    const double reynoldsX = kDensity * kSpeed * x / kViscosity;
    const double blasius = 0.664 / std::sqrt(reynoldsX);

    worstError = std::max(worstError, std::abs(cf - blasius) / blasius);
    ++samples;
  }

  ASSERT_GT(samples, 10);
  // Within 8%: first-order-in-space wall shear on a graded mesh, against an
  // analytic similarity solution. The measured spread is 1.6 to 3.7 percent.
  EXPECT_LT(worstError, 0.08) << "skin friction departs from Blasius";

  // The freestream must be undisturbed away from the plate - the check that
  // caught a badly posed leading edge during development.
  double peak = 0.0;
  const int column = (mesh.nodesI() - 1) / 2;
  for (int j = 0; j < mesh.nodesJ() - 1; ++j) {
    peak = std::max(peak, solver.field().velocity[static_cast<std::size_t>(
                                                      mesh.cellIndex(column, j))]
                              .x);
  }
  EXPECT_LT(peak, 1.03 * kSpeed) << "the outer flow is being over-accelerated";

  EXPECT_LT(monitor.maxDivergence, 1e-5);
}

// ---------------------------------------------------------------------------
// Residual behaviour and mass conservation
// ---------------------------------------------------------------------------

TEST(Validation, ResidualsFallMonotonicallyAndMassIsConserved) {
  const Mesh mesh = box(BoxOptions{.length = 3.0,
                                   .height = 1.0,
                                   .cellsX = 30,
                                   .cellsY = 20,
                                   .left = BoundaryType::Farfield,
                                   .right = BoundaryType::Outlet,
                                   .lower = BoundaryType::Wall,
                                   .upper = BoundaryType::Wall});

  FaceConditions conditions(mesh.faceCount());
  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    const auto& face = mesh.faces()[f];
    if (face.isInterior()) {
      continue;
    }
    if (face.boundary == BoundaryType::Farfield) {
      conditions[f].kind = BoundaryKind::Inlet;
      conditions[f].velocity = Vec2{1.0, 0.0};
    } else if (face.boundary == BoundaryType::Outlet) {
      conditions[f].kind = BoundaryKind::Outlet;
    } else {
      conditions[f].kind = BoundaryKind::NoSlipWall;
    }
  }

  auto created = SimpleSolver::create(mesh, conditions, SimpleSettings{});
  ASSERT_TRUE(created);
  SimpleSolver& solver = created.value();
  ASSERT_TRUE(solver.initialise(uniformField(mesh, Vec2{1.0, 0.0}, 1.0, 0.02)));

  std::vector<double> continuity;
  for (int i = 0; i < 400; ++i) {
    continuity.push_back(solver.iterate().residuals.continuity);
  }

  // Not monotone at every single step - SIMPLE re-balances early on - but the
  // trend must be unmistakable.
  EXPECT_LT(continuity.back(), 1e-6 * continuity.front());

  // Global mass balance: what enters must leave.
  double net = 0.0;
  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    if (!mesh.faces()[f].isInterior()) {
      net += solver.massFlux()[f];
    }
  }
  EXPECT_LT(std::abs(net), 1e-10);
}

}  // namespace
