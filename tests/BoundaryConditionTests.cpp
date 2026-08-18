// Tests for applying boundary conditions to a real mesh, and for the
// divergence check that validates the whole mesh-plus-field chain.

#include "cfd/flow/BoundaryConditions.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <string>

#include "cfd/geom/Airfoil.hpp"
#include "cfd/mesh/CGrid.hpp"

namespace {

using cfd::Vec2;
using cfd::flow::BoundaryConditions;
using cfd::flow::BoundaryKind;
using cfd::flow::continuityResidual;
using cfd::flow::divergence;
using cfd::flow::evaluateFaces;
using cfd::flow::FaceState;
using cfd::flow::FlowField;
using cfd::flow::FreestreamConditions;
using cfd::flow::maxAbsDivergence;
using cfd::mesh::BoundaryType;
using cfd::mesh::Mesh;

/// A coarse C-grid around NACA 2412 - small enough to be quick, real enough to
/// exercise every boundary the solver will meet.
const Mesh& sharedMesh() {
  static const Mesh mesh = [] {
    auto section = cfd::geom::makeNaca4Digit(
        "2412", {.chord = 1.0, .trailingEdge = cfd::geom::TrailingEdge::Closed});
    EXPECT_TRUE(section);
    auto grid = cfd::mesh::generateCGrid(section.value(),
                                         cfd::mesh::optionsFor(cfd::mesh::MeshResolution::Coarse));
    EXPECT_TRUE(grid) << (grid.hasError() ? grid.error().format() : "");
    return std::move(grid).value();
  }();
  return mesh;
}

FreestreamConditions defaultStream() {
  FreestreamConditions stream;
  stream.speed = 50.0;
  stream.angleOfAttackDeg = 4.0;
  stream.referencePressure = 0.0;
  return stream;
}

FlowField uniformField(const FreestreamConditions& stream) {
  auto field = FlowField::uniform(sharedMesh().cellCount(), stream, 1.0);
  EXPECT_TRUE(field);
  return std::move(field).value();
}

FaceState applied(const BoundaryConditions& conditions, const FreestreamConditions& stream) {
  auto faces = evaluateFaces(sharedMesh(), uniformField(stream), conditions, stream);
  EXPECT_TRUE(faces) << (faces.hasError() ? faces.error().format() : "");
  return std::move(faces).value();
}

// ---------------------------------------------------------------------------
// Patch mapping
// ---------------------------------------------------------------------------

TEST(BoundaryConditions, MapMeshPatchesOntoPhysics) {
  const BoundaryConditions conditions;

  EXPECT_EQ(BoundaryKind::NoSlipWall, conditions.kindFor(BoundaryType::Wall));
  EXPECT_EQ(BoundaryKind::FarField, conditions.kindFor(BoundaryType::Farfield));
  EXPECT_EQ(BoundaryKind::Outlet, conditions.kindFor(BoundaryType::Outlet));
  // The wake cut is never a physical boundary.
  EXPECT_EQ(BoundaryKind::Internal, conditions.kindFor(BoundaryType::WakeCut));
  EXPECT_EQ(BoundaryKind::Internal, conditions.kindFor(BoundaryType::Interior));
}

TEST(BoundaryConditions, DefaultsAreWellPosed) {
  EXPECT_TRUE(BoundaryConditions{}.validate().hasValue());
}

// With velocity imposed on every boundary, pressure is only determined up to a
// constant and the pressure equation is singular.
TEST(BoundaryConditions, RejectsHavingNoPressureAnchor) {
  BoundaryConditions conditions;
  conditions.outlet = BoundaryKind::Inlet;
  conditions.farField = BoundaryKind::Inlet;

  const auto status = conditions.validate();
  ASSERT_TRUE(status.hasError());
  EXPECT_NE(std::string::npos, status.error().message().find("pressure"));
}

TEST(BoundaryConditions, RejectsAWallThatFluidCouldPassThrough) {
  BoundaryConditions conditions;
  conditions.wall = BoundaryKind::Internal;
  EXPECT_TRUE(conditions.validate().hasError());
}

// ---------------------------------------------------------------------------
// What each condition imposes
// ---------------------------------------------------------------------------

TEST(EvaluateFaces, ProducesAValueOnEveryFace) {
  const FaceState faces = applied(BoundaryConditions{}, defaultStream());

  ASSERT_EQ(sharedMesh().faceCount(), faces.size());
  EXPECT_TRUE(faces.isConsistent());
  for (std::size_t f = 0; f < faces.size(); ++f) {
    ASSERT_TRUE(std::isfinite(faces.velocity[f].x)) << "face " << f;
    ASSERT_TRUE(std::isfinite(faces.velocity[f].y)) << "face " << f;
    ASSERT_TRUE(std::isfinite(faces.pressure[f])) << "face " << f;
  }
}

// The no-slip condition: both components vanish. This is the origin of the
// boundary layer and of every viscous effect this project cares about.
TEST(EvaluateFaces, WallFacesHaveZeroVelocity) {
  const Mesh& mesh = sharedMesh();
  const FaceState faces = applied(BoundaryConditions{}, defaultStream());

  std::size_t wallFaces = 0;
  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    if (mesh.faces()[f].boundary != BoundaryType::Wall) {
      continue;
    }
    ++wallFaces;
    EXPECT_EQ(BoundaryKind::NoSlipWall, faces.kind[f]);
    EXPECT_NEAR(0.0, faces.velocity[f].x, 1e-15) << "face " << f;
    EXPECT_NEAR(0.0, faces.velocity[f].y, 1e-15) << "face " << f;
  }
  EXPECT_GT(wallFaces, 0u);
}

TEST(EvaluateFaces, InletFacesCarryTheFreestreamVelocity) {
  BoundaryConditions conditions;
  conditions.farField = BoundaryKind::Inlet;
  const FreestreamConditions stream = defaultStream();
  const Mesh& mesh = sharedMesh();
  const FaceState faces = applied(conditions, stream);

  const Vec2 expected = stream.velocity();
  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    if (mesh.faces()[f].boundary != BoundaryType::Farfield) {
      continue;
    }
    EXPECT_EQ(BoundaryKind::Inlet, faces.kind[f]);
    EXPECT_NEAR(expected.x, faces.velocity[f].x, 1e-12);
    EXPECT_NEAR(expected.y, faces.velocity[f].y, 1e-12);
    EXPECT_EQ(1, faces.inflow[f]);
  }
}

TEST(EvaluateFaces, OutletFacesCarryTheReferencePressure) {
  FreestreamConditions stream = defaultStream();
  stream.referencePressure = 1234.5;
  const Mesh& mesh = sharedMesh();
  const FaceState faces = applied(BoundaryConditions{}, stream);

  std::size_t outletFaces = 0;
  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    if (mesh.faces()[f].boundary != BoundaryType::Outlet) {
      continue;
    }
    ++outletFaces;
    EXPECT_EQ(BoundaryKind::Outlet, faces.kind[f]);
    EXPECT_NEAR(1234.5, faces.pressure[f], 1e-9);
    // Velocity is extrapolated, not imposed.
    const auto owner = static_cast<std::size_t>(mesh.faces()[f].owner);
    EXPECT_NEAR(stream.velocity().x, faces.velocity[f].x, 1e-12) << owner;
  }
  EXPECT_GT(outletFaces, 0u);
}

// A boundary the stream runs parallel to must not be treated as an outlet.
//
// At exactly zero incidence the top and bottom of a C-grid have normals of
// (0, +/-1) and the stream is (U, 0), so u.n is exactly zero along all of them.
// Classifying by the bare sign puts every one of those faces on the outflow
// side, where a pressure is imposed and the velocity left free - turning a
// boundary the flow merely slides past into a surface mass can breathe through.
// In the solver, where the same decision is remade every iteration from the
// current flux, that made the boundary condition flip back and forth and the
// continuity residual never converged. Far from the body the flow *is* the
// freestream, so a tangential face takes it.
TEST(EvaluateFaces, ABoundaryTheStreamIsParallelToTakesTheFreestream) {
  const Mesh& mesh = sharedMesh();
  FreestreamConditions stream = defaultStream();
  stream.angleOfAttackDeg = 0.0;

  const FaceState faces = applied(BoundaryConditions{}, stream);

  std::size_t tangential = 0;
  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    if (mesh.faces()[f].boundary != BoundaryType::Farfield) {
      continue;
    }
    const double outward = dot(stream.velocity(), mesh.faceNormals()[f]);
    if (std::abs(outward) > 1e-6 * stream.speed) {
      continue;
    }
    ++tangential;
    EXPECT_NE(0, faces.inflow[f])
        << "face " << f << " is tangential (u.n = " << outward << ") but is an outlet";
    EXPECT_NEAR(stream.velocity().x, faces.velocity[f].x, 1e-12);
    EXPECT_NEAR(stream.velocity().y, faces.velocity[f].y, 1e-12);
  }
  // The case only means anything if such faces exist on this mesh.
  EXPECT_GT(tangential, 0u);
}

// An external-flow outer boundary is not all inflow: the stream enters at the
// front and leaves at the back, and which is which follows the sign of u.n.
TEST(EvaluateFaces, FarFieldSplitsIntoInflowAndOutflow) {
  const Mesh& mesh = sharedMesh();
  const FreestreamConditions stream = defaultStream();
  const FaceState faces = applied(BoundaryConditions{}, stream);

  std::size_t entering = 0;
  std::size_t leaving = 0;
  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    if (mesh.faces()[f].boundary != BoundaryType::Farfield) {
      continue;
    }
    const double outward = dot(stream.velocity(), mesh.faceNormals()[f]);
    if (faces.inflow[f] != 0) {
      ++entering;
      // Inflow covers "entering" and "running parallel to"; only fluid that is
      // unambiguously leaving gets a pressure condition.
      EXPECT_LT(outward, 1e-3 * stream.speed)
          << "face " << f << " marked inflow but the stream is clearly leaving";
      // Inflow faces have the stream imposed on them.
      EXPECT_NEAR(stream.velocity().x, faces.velocity[f].x, 1e-12);
    } else {
      ++leaving;
      EXPECT_GE(outward, 0.0) << "face " << f << " marked outflow but u.n < 0";
      EXPECT_NEAR(stream.referencePressure, faces.pressure[f], 1e-12);
    }
  }
  // Both must occur: the front of the C takes fluid in, the top and bottom let
  // it out again.
  EXPECT_GT(entering, 0u);
  EXPECT_GT(leaving, 0u);
}

// The wake cut is a slit, not a wall. Its faces must be interpolated from the
// cells on both sides, exactly like interior faces.
TEST(EvaluateFaces, WakeCutFacesAreTreatedAsInterior) {
  const Mesh& mesh = sharedMesh();
  const FreestreamConditions stream = defaultStream();
  const FaceState faces = applied(BoundaryConditions{}, stream);

  std::size_t cutFaces = 0;
  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    if (mesh.faces()[f].boundary != BoundaryType::WakeCut) {
      continue;
    }
    ++cutFaces;
    EXPECT_EQ(BoundaryKind::Internal, faces.kind[f]);
    // Not clamped to zero the way a wall would be.
    EXPECT_NEAR(stream.velocity().x, faces.velocity[f].x, 1e-12) << "face " << f;
    EXPECT_NEAR(stream.velocity().y, faces.velocity[f].y, 1e-12) << "face " << f;
  }
  EXPECT_GT(cutFaces, 0u);
}

TEST(EvaluateFaces, InteriorFacesInterpolateTheCellsTheySeparate) {
  const Mesh& mesh = sharedMesh();
  const FreestreamConditions stream = defaultStream();
  const FaceState faces = applied(BoundaryConditions{}, stream);

  // With a uniform field any sane interpolation must reproduce it exactly.
  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    if (!mesh.faces()[f].isInterior()) {
      continue;
    }
    EXPECT_NEAR(stream.velocity().x, faces.velocity[f].x, 1e-12) << "face " << f;
    EXPECT_NEAR(stream.velocity().y, faces.velocity[f].y, 1e-12) << "face " << f;
  }
}

TEST(EvaluateFaces, RejectsAFieldThatDoesNotMatchTheMesh) {
  FlowField wrongSize;
  wrongSize.resize(7);
  const auto result = evaluateFaces(sharedMesh(), wrongSize, BoundaryConditions{},
                                    defaultStream());
  EXPECT_TRUE(result.hasError());
}

// ---------------------------------------------------------------------------
// Divergence
// ---------------------------------------------------------------------------

// The sharpest check in this phase. A uniform velocity has zero divergence
// analytically, and discretely the net flux out of a cell is
// u . sum(n*A) = u . 0 = 0 because the outward area vectors of a closed cell
// sum to zero. So this simultaneously exercises the mesh metrics, the face
// interpolation and the flux sign convention - if any of them is wrong, this
// is not zero.
TEST(Divergence, UniformFlowIsDivergenceFreeWhenNoWallInterferes) {
  const Mesh& mesh = sharedMesh();
  const FreestreamConditions stream = defaultStream();

  // Far field everywhere, including the "wall", so nothing disturbs the
  // uniform field and the whole domain should balance.
  BoundaryConditions conditions;
  conditions.wall = BoundaryKind::FarField;

  const FaceState faces = applied(conditions, stream);
  auto result = divergence(mesh, faces);
  ASSERT_TRUE(result) << (result.hasError() ? result.error().format() : "");

  // Scale for comparison: a cell of area A with sides ~sqrt(A) carrying speed U
  // has fluxes of order U/sqrt(A) in these units, so compare against U over the
  // smallest length in the mesh.
  const double scale = stream.speed / std::sqrt(mesh.quality().minCellArea);
  EXPECT_LT(maxAbsDivergence(result.value()), 1e-9 * scale);
}

// With the wall switched back on, the balance must fail - and only next to the
// wall. That is not a defect: it is precisely why a solver is needed. The
// uniform guess satisfies the far field everywhere and the wall nowhere, and
// the iteration's job is to fix that.
TEST(Divergence, NoSlipWallBreaksTheBalanceOnlyNextToTheWall) {
  const Mesh& mesh = sharedMesh();
  const FaceState faces = applied(BoundaryConditions{}, defaultStream());
  auto result = divergence(mesh, faces);
  ASSERT_TRUE(result);
  const std::vector<double>& div = result.value();

  // Mark the cells that own a wall face.
  std::vector<char> touchesWall(mesh.cellCount(), 0);
  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    if (mesh.faces()[f].boundary == BoundaryType::Wall) {
      touchesWall[static_cast<std::size_t>(mesh.faces()[f].owner)] = 1;
    }
  }

  double worstAtWall = 0.0;
  double worstAwayFromWall = 0.0;
  for (std::size_t c = 0; c < div.size(); ++c) {
    if (touchesWall[c] != 0) {
      worstAtWall = std::max(worstAtWall, std::abs(div[c]));
    } else {
      worstAwayFromWall = std::max(worstAwayFromWall, std::abs(div[c]));
    }
  }

  EXPECT_GT(worstAtWall, 0.0) << "no-slip must unbalance the cells it touches";
  EXPECT_GT(worstAtWall, worstAwayFromWall * 1e3)
      << "the imbalance should be confined to the wall";
}

TEST(Divergence, ScalesWithFreestreamSpeed) {
  const Mesh& mesh = sharedMesh();

  const auto worstFor = [&mesh](double speed) {
    FreestreamConditions stream = defaultStream();
    stream.speed = speed;
    const FaceState faces = applied(BoundaryConditions{}, stream);
    auto result = divergence(mesh, faces);
    EXPECT_TRUE(result);
    return maxAbsDivergence(result.value());
  };

  // Doubling the speed doubles every flux, so it doubles the imbalance.
  EXPECT_NEAR(2.0 * worstFor(25.0), worstFor(50.0), 1e-6 * worstFor(50.0));
}

TEST(Divergence, RejectsFaceStateThatDoesNotMatchTheMesh) {
  FaceState wrong;
  wrong.velocity.resize(3);
  wrong.pressure.resize(3);
  wrong.kind.resize(3);
  wrong.inflow.resize(3);
  EXPECT_TRUE(divergence(sharedMesh(), wrong).hasError());
}

TEST(ContinuityResidual, IsZeroForABalancedFieldAndPositiveOtherwise) {
  const Mesh& mesh = sharedMesh();
  const FreestreamConditions stream = defaultStream();

  BoundaryConditions noWall;
  noWall.wall = BoundaryKind::FarField;
  auto balanced = divergence(mesh, applied(noWall, stream));
  ASSERT_TRUE(balanced);
  const double balancedResidual = continuityResidual(mesh, balanced.value()).continuity;

  auto unbalanced = divergence(mesh, applied(BoundaryConditions{}, stream));
  ASSERT_TRUE(unbalanced);
  const double unbalancedResidual = continuityResidual(mesh, unbalanced.value()).continuity;

  EXPECT_LT(balancedResidual, 1e-12 * stream.speed);
  EXPECT_GT(unbalancedResidual, balancedResidual);
}

// Nothing has solved a momentum equation, so those residuals must read zero
// rather than some plausible-looking number.
TEST(ContinuityResidual, LeavesMomentumEntriesUntouched) {
  const Mesh& mesh = sharedMesh();
  auto div = divergence(mesh, applied(BoundaryConditions{}, defaultStream()));
  ASSERT_TRUE(div);

  const auto residuals = continuityResidual(mesh, div.value());
  EXPECT_DOUBLE_EQ(0.0, residuals.momentumX);
  EXPECT_DOUBLE_EQ(0.0, residuals.momentumY);
}

}  // namespace
