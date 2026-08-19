// Tests for the turbulence closure and the interface it sits behind.
//
// Two things are being checked, and they are different in kind.
//
// The interface: that the solver depends on the abstraction and not on any
// particular model. The sharpest form of that is a control - solving with the
// laminar closure attached must give bit-identical results to solving with no
// closure at all, because "the flow is laminar" is a model that adds nothing.
// If those two ever differ, the solver has grown a dependency on which model is
// attached.
//
// The model: that k-omega SST reproduces the relations it is defined by. Its
// constants, its eddy-viscosity formula, its wall treatment and its limiter are
// all things that can be stated exactly and checked exactly, without needing a
// converged flow to compare against.

#include "cfd/solver/KOmegaSST.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <memory>

#include "cfd/flow/BoundaryConditions.hpp"
#include "cfd/geom/Airfoil.hpp"
#include "cfd/mesh/CGrid.hpp"
#include "cfd/solver/SimpleSolver.hpp"
#include "cfd/solver/TurbulenceModel.hpp"

namespace {

using cfd::Vec2;
using cfd::flow::FlowField;
using cfd::flow::FreestreamConditions;
using cfd::mesh::Mesh;
using cfd::solver::KOmegaSST;
using cfd::solver::LaminarModel;
using cfd::solver::SimpleSolver;
using cfd::solver::SimpleSettings;
using cfd::solver::SSTConstants;
using cfd::solver::TurbulenceInflow;
using cfd::solver::wallDistances;

/// A small C-grid, shared so the solved cases stay quick.
const Mesh& sharedMesh() {
  static const Mesh mesh = [] {
    auto section = cfd::geom::makeNaca4Digit(
        "0012", {.chord = 1.0, .trailingEdge = cfd::geom::TrailingEdge::Closed});
    EXPECT_TRUE(section);
    cfd::mesh::CGridOptions options =
        cfd::mesh::optionsFor(cfd::mesh::MeshResolution::Coarse);
    options.surfacePoints = 40;
    options.wakePoints = 20;
    options.normalPoints = 28;
    auto grid = cfd::mesh::generateCGrid(section.value(), options);
    EXPECT_TRUE(grid) << (grid.hasError() ? grid.error().format() : "");
    return std::move(grid).value();
  }();
  return mesh;
}

FreestreamConditions stream(double reynolds = 1.0e6) {
  FreestreamConditions conditions;
  conditions.speed = 1.0;
  conditions.density = 1.0;
  conditions.reynoldsNumber = reynolds;
  conditions.angleOfAttackDeg = 0.0;
  return conditions;
}

cfd::flow::FaceConditions faces(const Mesh& mesh, const FreestreamConditions& conditions) {
  auto built = cfd::flow::buildFaceConditions(mesh, cfd::flow::BoundaryConditions{}, conditions);
  EXPECT_TRUE(built);
  return std::move(built).value();
}

FlowField uniform(const Mesh& mesh, const FreestreamConditions& conditions) {
  auto field = FlowField::uniform(mesh.cellCount(), conditions, 1.0);
  EXPECT_TRUE(field);
  return std::move(field).value();
}

// ---------------------------------------------------------------------------
// The interface
// ---------------------------------------------------------------------------

// The control. "The flow is laminar" is a closure that supplies no Reynolds
// stresses, so attaching it must change nothing at all. If this ever fails, the
// solver has grown a dependency on whether a model is present rather than on
// what one returns.
TEST(Turbulence, TheLaminarClosureIsIndistinguishableFromNoClosure) {
  const Mesh& mesh = sharedMesh();
  const FreestreamConditions conditions = stream(500.0);

  const auto run = [&](bool attach) {
    auto created = SimpleSolver::create(mesh, faces(mesh, conditions), SimpleSettings{});
    EXPECT_TRUE(created);
    if (attach) {
      EXPECT_TRUE(created.value().setTurbulenceModel(std::make_unique<LaminarModel>()));
    }
    EXPECT_TRUE(created.value().initialise(uniform(mesh, conditions)));
    for (int i = 0; i < 40; ++i) {
      created.value().iterate();
    }
    return created.value().field();
  };

  const FlowField without = run(false);
  const FlowField with = run(true);

  ASSERT_EQ(without.size(), with.size());
  for (std::size_t c = 0; c < without.size(); ++c) {
    EXPECT_DOUBLE_EQ(without.velocity[c].x, with.velocity[c].x) << "cell " << c;
    EXPECT_DOUBLE_EQ(without.velocity[c].y, with.velocity[c].y) << "cell " << c;
    EXPECT_DOUBLE_EQ(without.pressure[c], with.pressure[c]) << "cell " << c;
    // And the effective viscosity is still purely molecular.
    EXPECT_DOUBLE_EQ(without.viscosity[c], with.viscosity[c]) << "cell " << c;
  }
}

// The interface is what the solver holds; the concrete type is not.
TEST(Turbulence, AModelCanBeSwappedThroughTheInterface) {
  const Mesh& mesh = sharedMesh();
  const FreestreamConditions conditions = stream();

  auto created = SimpleSolver::create(mesh, faces(mesh, conditions), SimpleSettings{});
  ASSERT_TRUE(created);
  SimpleSolver& solver = created.value();

  EXPECT_EQ(nullptr, solver.turbulenceModel());

  ASSERT_TRUE(solver.setTurbulenceModel(std::make_unique<LaminarModel>()));
  ASSERT_NE(nullptr, solver.turbulenceModel());
  EXPECT_EQ("laminar", solver.turbulenceModel()->name());

  ASSERT_TRUE(solver.setTurbulenceModel(std::make_unique<KOmegaSST>()));
  ASSERT_NE(nullptr, solver.turbulenceModel());
  EXPECT_EQ("k-omega SST", solver.turbulenceModel()->name());

  // And back to nothing, which must restore purely molecular viscosity.
  ASSERT_TRUE(solver.initialise(uniform(mesh, conditions)));
  ASSERT_TRUE(solver.setTurbulenceModel(nullptr));
  EXPECT_EQ(nullptr, solver.turbulenceModel());
  for (const double mu : solver.field().viscosity) {
    EXPECT_GT(mu, 0.0);
  }
}

// ---------------------------------------------------------------------------
// Wall distance
// ---------------------------------------------------------------------------

TEST(Turbulence, WallDistanceIsSmallestAtTheWallAndGrowsOutwards) {
  const Mesh& mesh = sharedMesh();
  const FreestreamConditions conditions = stream();
  const std::vector<double> distance = wallDistances(mesh, faces(mesh, conditions));

  ASSERT_EQ(mesh.cellCount(), distance.size());
  for (const double d : distance) {
    EXPECT_GE(d, 0.0);
    EXPECT_TRUE(std::isfinite(d));
  }

  // The cell owning a wall face is the closest thing to that wall there is.
  const cfd::flow::FaceConditions applied = faces(mesh, conditions);
  double nearestToAnyWall = 1e30;
  double farthest = 0.0;
  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    if (applied[f].kind != cfd::flow::BoundaryKind::NoSlipWall) {
      continue;
    }
    nearestToAnyWall =
        std::min(nearestToAnyWall, distance[static_cast<std::size_t>(mesh.faces()[f].owner)]);
  }
  for (const double d : distance) {
    farthest = std::max(farthest, d);
  }

  EXPECT_LT(nearestToAnyWall, 0.01) << "the first cell should be hard against the wall";
  EXPECT_GT(farthest, 1.0) << "the far field should be chords away";
}

TEST(Turbulence, WallDistanceIsLargeWhenThereIsNoWall) {
  const Mesh& mesh = sharedMesh();
  cfd::flow::FaceConditions none(mesh.faceCount());
  for (auto& condition : none) {
    condition.kind = cfd::flow::BoundaryKind::FarField;
  }

  const std::vector<double> distance = wallDistances(mesh, none);
  ASSERT_EQ(mesh.cellCount(), distance.size());
  for (const double d : distance) {
    EXPECT_GT(d, 1.0e3) << "no wall means no wall damping, not zero distance";
    EXPECT_TRUE(std::isfinite(d));
  }
}

// ---------------------------------------------------------------------------
// The model's own definitions
// ---------------------------------------------------------------------------

// gamma_i = beta_i/beta* - sigma_omega_i kappa^2/sqrt(beta*) is what makes the
// model reproduce the log law; the published values are 0.5532 and 0.4403.
TEST(Turbulence, TheBlendedConstantsMatchThePublishedValues) {
  const SSTConstants c;
  EXPECT_NEAR(0.5532, c.gamma1(), 1e-4);
  EXPECT_NEAR(0.4403, c.gamma2(), 1e-4);
}

TEST(Turbulence, InflowConditionsAreValidated) {
  TurbulenceInflow inflow;
  EXPECT_TRUE(inflow.validate());

  inflow.intensity = -0.1;
  EXPECT_FALSE(inflow.validate());

  inflow = TurbulenceInflow{};
  inflow.viscosityRatio = 0.0;
  EXPECT_FALSE(inflow.validate());
}

TEST(Turbulence, FreestreamValuesFollowFromIntensityAndViscosityRatio) {
  const Mesh& mesh = sharedMesh();
  const FreestreamConditions conditions = stream();
  const FlowField field = uniform(mesh, conditions);

  TurbulenceInflow inflow;
  inflow.intensity = 0.01;
  inflow.viscosityRatio = 5.0;

  KOmegaSST model;
  ASSERT_TRUE(model.initialise(mesh, faces(mesh, conditions), field, inflow));

  // k = (3/2)(I U)^2 with U = 1.
  EXPECT_NEAR(1.5 * 0.01 * 0.01, model.freestreamEnergy(), 1e-12);

  // omega = rho k / mu_t, with mu_t the requested multiple of the molecular
  // viscosity. Inverting mu_t = rho k / omega is the whole of it.
  const double mu = field.viscosity[0];
  EXPECT_NEAR(field.density[0] * model.freestreamEnergy() / (5.0 * mu),
              model.freestreamDissipation(), 1e-9 * model.freestreamDissipation());

  // Out in the freestream the eddy viscosity starts at that multiple. Near a
  // wall it does not, and should not: omega is seeded from the analytic
  // sublayer solution there, which is orders of magnitude larger than the
  // freestream value, so mu_t = rho k / omega starts correspondingly smaller.
  // Starting it uniform is what used to blow the solve up in its first hundred
  // iterations.
  const std::vector<double>& distance = model.wallDistance();
  std::size_t freestreamCells = 0;
  for (std::size_t c = 0; c < model.eddyViscosity().size(); ++c) {
    if (distance[c] > 1.0) {
      EXPECT_NEAR(5.0 * mu, model.eddyViscosity()[c], 1e-12 * mu) << "cell " << c;
      ++freestreamCells;
    } else {
      EXPECT_LE(model.eddyViscosity()[c], 5.0 * mu * (1.0 + 1e-12)) << "cell " << c;
      EXPECT_GT(model.eddyViscosity()[c], 0.0) << "cell " << c;
    }
  }
  EXPECT_GT(freestreamCells, 100u);
}

// Away from a wall and at low strain the limiter is inactive, and the model
// reduces to its defining relation mu_t = rho k / omega.
TEST(Turbulence, EddyViscosityIsDensityTimesEnergyOverDissipation) {
  const Mesh& mesh = sharedMesh();
  const FreestreamConditions conditions = stream();
  const FlowField field = uniform(mesh, conditions);

  KOmegaSST model;
  ASSERT_TRUE(model.initialise(mesh, faces(mesh, conditions), field, TurbulenceInflow{}));

  const std::vector<double>& k = model.turbulentEnergy();
  const std::vector<double>& omega = model.specificDissipation();
  const std::vector<double>& mut = model.eddyViscosity();

  ASSERT_EQ(mesh.cellCount(), mut.size());
  for (std::size_t c = 0; c < mut.size(); ++c) {
    EXPECT_NEAR(field.density[c] * k[c] / omega[c], mut[c], 1e-9 * mut[c]) << "cell " << c;
  }
}

// omega is singular at a wall, so it is not extrapolated but set outright from
// the analytic sublayer solution, omega = 60 nu / (beta1 d^2).
TEST(Turbulence, TheWallValueOfOmegaFollowsTheSublayerSolution) {
  const Mesh& mesh = sharedMesh();
  const FreestreamConditions conditions = stream();
  const cfd::flow::FaceConditions applied = faces(mesh, conditions);
  const FlowField field = uniform(mesh, conditions);

  auto created = SimpleSolver::create(mesh, applied, SimpleSettings{});
  ASSERT_TRUE(created);
  ASSERT_TRUE(created.value().setTurbulenceModel(std::make_unique<KOmegaSST>()));
  ASSERT_TRUE(created.value().initialise(field));
  created.value().iterate();

  const auto* model = dynamic_cast<const KOmegaSST*>(created.value().turbulenceModel());
  ASSERT_NE(nullptr, model);

  const SSTConstants constants;
  const std::vector<double>& omega = model->specificDissipation();
  const std::vector<double>& distance = model->wallDistance();

  std::size_t checked = 0;
  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    if (applied[f].kind != cfd::flow::BoundaryKind::NoSlipWall) {
      continue;
    }
    const auto owner = static_cast<std::size_t>(mesh.faces()[f].owner);
    const double nu = field.viscosity[owner] / field.density[owner];
    const double d = distance[owner];
    // The viscous-sublayer asymptote blended with the log-layer form. The
    // coefficient is 6: the 60 in Menter's paper belongs at the wall *face*,
    // not in the first cell.
    const double viscous = 6.0 * nu / (constants.beta1 * d * d);
    const double logarithmic = std::sqrt(std::max(model->turbulentEnergy()[owner], 0.0)) /
                               (std::pow(constants.betaStar, 0.25) * constants.kappa * d);
    const double expected = std::sqrt(viscous * viscous + logarithmic * logarithmic);
    EXPECT_NEAR(expected, omega[owner], 1e-6 * expected) << "wall face " << f;
    // k is solved in the first cell, not imposed: the fluctuations vanish *at
    // the wall*, which is a condition on the face, not on the cell one layer
    // out into the fluid. All that can be said after a single iteration is that
    // it is a physical number; what it converges to is checked below.
    EXPECT_GE(model->turbulentEnergy()[owner], 0.0);
    EXPECT_TRUE(std::isfinite(model->turbulentEnergy()[owner]));
    ++checked;
  }
  EXPECT_GT(checked, 10u);
}

// The blending function is what makes this SST rather than either parent model:
// 1 deep inside the boundary layer, 0 out in the freestream.
TEST(Turbulence, BlendingIsOneAtTheWallAndFallsAwayFromIt) {
  const Mesh& mesh = sharedMesh();
  const FreestreamConditions conditions = stream();
  const cfd::flow::FaceConditions applied = faces(mesh, conditions);

  auto created = SimpleSolver::create(mesh, applied, SimpleSettings{});
  ASSERT_TRUE(created);
  ASSERT_TRUE(created.value().setTurbulenceModel(std::make_unique<KOmegaSST>()));
  ASSERT_TRUE(created.value().initialise(uniform(mesh, conditions)));
  created.value().iterate();

  const auto* model = dynamic_cast<const KOmegaSST*>(created.value().turbulenceModel());
  ASSERT_NE(nullptr, model);

  const std::vector<double>& f1 = model->blending();
  const std::vector<double>& d = model->wallDistance();

  double nearWall = 0.0;
  double farField = 1.0;
  for (std::size_t c = 0; c < f1.size(); ++c) {
    EXPECT_GE(f1[c], 0.0);
    EXPECT_LE(f1[c], 1.0);
    if (d[c] < 1e-3) {
      nearWall = std::max(nearWall, f1[c]);
    }
    if (d[c] > 5.0) {
      farField = std::min(farField, f1[c]);
    }
  }
  EXPECT_GT(nearWall, 0.9) << "F1 should be ~1 in the sublayer";
  EXPECT_LT(farField, 0.5) << "F1 should fall away out in the freestream";
}

// The headline behavioural check: attaching the model must actually produce
// turbulent mixing, and it must stay bounded and positive while doing so.
TEST(Turbulence, TheModelProducesBoundedPositiveEddyViscosity) {
  const Mesh& mesh = sharedMesh();
  const FreestreamConditions conditions = stream();

  auto created = SimpleSolver::create(mesh, faces(mesh, conditions), SimpleSettings{});
  ASSERT_TRUE(created);
  ASSERT_TRUE(created.value().setTurbulenceModel(std::make_unique<KOmegaSST>()));
  ASSERT_TRUE(created.value().initialise(uniform(mesh, conditions)));

  for (int i = 0; i < 200; ++i) {
    const auto monitor = created.value().iterate();
    ASSERT_TRUE(std::isfinite(monitor.residuals.continuity)) << "diverged at " << i;
    ASSERT_TRUE(std::isfinite(monitor.maxEddyViscosityRatio)) << "mu_t blew up at " << i;
  }

  const auto* model = dynamic_cast<const KOmegaSST*>(created.value().turbulenceModel());
  ASSERT_NE(nullptr, model);

  for (const double k : model->turbulentEnergy()) {
    EXPECT_GE(k, 0.0) << "turbulent energy went negative";
    EXPECT_TRUE(std::isfinite(k));
  }
  for (const double omega : model->specificDissipation()) {
    EXPECT_GT(omega, 0.0) << "specific dissipation went non-positive";
    EXPECT_TRUE(std::isfinite(omega));
  }
  for (const double mut : model->eddyViscosity()) {
    EXPECT_GE(mut, 0.0);
    EXPECT_TRUE(std::isfinite(mut));
  }

  // And it is doing something: at Re = 10^6 the boundary layer should be mixing
  // far harder than molecular viscosity alone.
  const auto monitor = created.value().iterate();
  EXPECT_GT(monitor.maxEddyViscosityRatio, 1.0)
      << "the model is attached but not producing any turbulent mixing";

  // A boundary layer is far *more* turbulent than the stream it grew out of -
  // that is what a boundary layer is. Freestream k here is 1.5(0.001 U)^2,
  // while the near-wall value is orders of magnitude above it.
  double nearWallPeak = 0.0;
  const std::vector<double>& distance = model->wallDistance();
  for (std::size_t c = 0; c < distance.size(); ++c) {
    if (distance[c] < 0.01) {
      nearWallPeak = std::max(nearWallPeak, model->turbulentEnergy()[c]);
    }
  }
  EXPECT_GT(nearWallPeak, 10.0 * model->freestreamEnergy())
      << "the boundary layer is no more turbulent than the freestream";
}

// The production limiter exists to stop a stagnation point - high strain, no
// real production - manufacturing turbulence that then convects downstream.
TEST(Turbulence, ProductionIsLimitedRelativeToDissipation) {
  const Mesh& mesh = sharedMesh();
  const FreestreamConditions conditions = stream();

  SSTConstants tight;
  tight.productionLimit = 1.0;  // far stricter than the standard 10

  auto strict = SimpleSolver::create(mesh, faces(mesh, conditions), SimpleSettings{});
  ASSERT_TRUE(strict);
  ASSERT_TRUE(strict.value().setTurbulenceModel(std::make_unique<KOmegaSST>(tight)));
  ASSERT_TRUE(strict.value().initialise(uniform(mesh, conditions)));

  auto standard = SimpleSolver::create(mesh, faces(mesh, conditions), SimpleSettings{});
  ASSERT_TRUE(standard);
  ASSERT_TRUE(standard.value().setTurbulenceModel(std::make_unique<KOmegaSST>()));
  ASSERT_TRUE(standard.value().initialise(uniform(mesh, conditions)));

  double strictPeak = 0.0;
  double standardPeak = 0.0;
  for (int i = 0; i < 120; ++i) {
    strictPeak = strict.value().iterate().maxEddyViscosityRatio;
    standardPeak = standard.value().iterate().maxEddyViscosityRatio;
  }

  // A tighter cap on production cannot produce *more* turbulence.
  EXPECT_LE(strictPeak, standardPeak * 1.001)
      << "limiting production harder made more eddy viscosity, which is backwards";
}

}  // namespace
