// Tests for the freestream operating point and the cell-centred state.

#include "cfd/flow/FlowField.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>

#include "cfd/flow/Freestream.hpp"

namespace {

using cfd::Vec2;
using cfd::flow::FlowField;
using cfd::flow::FreestreamConditions;
using cfd::flow::ResidualHistory;
using cfd::flow::ResidualSet;
using cfd::flow::TimeState;

// ---------------------------------------------------------------------------
// Freestream
// ---------------------------------------------------------------------------

TEST(Freestream, VelocityFollowsTheAngleOfAttack) {
  FreestreamConditions stream;
  stream.speed = 50.0;

  stream.angleOfAttackDeg = 0.0;
  EXPECT_NEAR(50.0, stream.velocity().x, 1e-12);
  EXPECT_NEAR(0.0, stream.velocity().y, 1e-12);

  // Positive incidence tilts the oncoming flow upwards relative to the chord.
  stream.angleOfAttackDeg = 90.0;
  EXPECT_NEAR(0.0, stream.velocity().x, 1e-12);
  EXPECT_NEAR(50.0, stream.velocity().y, 1e-12);

  stream.angleOfAttackDeg = 30.0;
  EXPECT_NEAR(50.0 * std::cos(std::numbers::pi / 6.0), stream.velocity().x, 1e-12);
  EXPECT_NEAR(50.0 * std::sin(std::numbers::pi / 6.0), stream.velocity().y, 1e-12);
}

TEST(Freestream, VelocityMagnitudeIsTheSpeedAtAnyIncidence) {
  FreestreamConditions stream;
  stream.speed = 37.5;
  for (const double alpha : {-20.0, -5.0, 0.0, 8.0, 45.0}) {
    stream.angleOfAttackDeg = alpha;
    EXPECT_NEAR(37.5, length(stream.velocity()), 1e-12) << "at " << alpha << " deg";
  }
}

TEST(Freestream, DynamicPressureIsHalfRhoVSquared) {
  FreestreamConditions stream;
  stream.speed = 40.0;
  stream.density = 1.2;

  EXPECT_NEAR(0.5 * 1.2 * 1600.0, stream.dynamicPressure(), 1e-12);
}

// The whole point of parameterising by Reynolds number: viscosity is whatever
// it takes to hit it.
TEST(Freestream, ViscosityIsDerivedFromTheReynoldsNumber) {
  FreestreamConditions stream;
  stream.speed = 50.0;
  stream.density = 1.225;
  stream.reynoldsNumber = 1.0e6;

  const double mu = stream.dynamicViscosity(1.0);
  EXPECT_NEAR(1.225 * 50.0 / 1.0e6, mu, 1e-12 * mu);

  // Round trip: the derived viscosity must reproduce the requested Re.
  EXPECT_NEAR(1.0e6, stream.density * stream.speed * 1.0 / mu, 1.0);
}

TEST(Freestream, ViscosityScalesWithChord) {
  FreestreamConditions stream;
  // A bigger chord at the same Reynolds number means a thicker fluid. Compared
  // with a relative tolerance: these values are around 1e-4, where an absolute
  // tolerance small enough to look strict would be below one ULP.
  const double expected = 3.0 * stream.dynamicViscosity(1.0);
  EXPECT_NEAR(expected, stream.dynamicViscosity(3.0), 1e-12 * expected);
}

TEST(Freestream, KinematicViscosityIsDynamicOverDensity) {
  FreestreamConditions stream;
  const double expected = stream.dynamicViscosity(1.0) / stream.density;
  EXPECT_NEAR(expected, stream.kinematicViscosity(1.0), 1e-12 * expected);
}

TEST(Freestream, RejectsUnphysicalConditions) {
  EXPECT_TRUE(FreestreamConditions{.speed = 0.0}.validate().hasError());
  EXPECT_TRUE(FreestreamConditions{.speed = -10.0}.validate().hasError());
  EXPECT_TRUE(FreestreamConditions{.density = 0.0}.validate().hasError());
  EXPECT_TRUE(FreestreamConditions{.angleOfAttackDeg = 120.0}.validate().hasError());
  EXPECT_TRUE(FreestreamConditions{.reynoldsNumber = 0.0}.validate().hasError());
  EXPECT_TRUE(FreestreamConditions{}.validate().hasValue());
}

// ---------------------------------------------------------------------------
// FlowField
// ---------------------------------------------------------------------------

TEST(FlowField, UniformInitialisationFillsEveryCellWithTheStream) {
  FreestreamConditions stream;
  stream.speed = 60.0;
  stream.angleOfAttackDeg = 5.0;
  stream.referencePressure = 101325.0;

  auto result = FlowField::uniform(128, stream, 1.0);
  ASSERT_TRUE(result) << (result.hasError() ? result.error().format() : "");
  const FlowField& field = result.value();

  ASSERT_EQ(128u, field.size());
  EXPECT_TRUE(field.isConsistent());

  const Vec2 expected = stream.velocity();
  for (std::size_t c = 0; c < field.size(); ++c) {
    EXPECT_NEAR(expected.x, field.velocity[c].x, 1e-12);
    EXPECT_NEAR(expected.y, field.velocity[c].y, 1e-12);
    EXPECT_NEAR(101325.0, field.pressure[c], 1e-9);
    EXPECT_NEAR(stream.density, field.density[c], 1e-12);
    EXPECT_DOUBLE_EQ(stream.dynamicViscosity(1.0), field.viscosity[c]);
  }
}

TEST(FlowField, ResizeZeroesEverythingAndStaysConsistent) {
  FlowField field;
  field.resize(10);

  EXPECT_EQ(10u, field.size());
  EXPECT_TRUE(field.isConsistent());
  for (std::size_t c = 0; c < field.size(); ++c) {
    EXPECT_DOUBLE_EQ(0.0, field.velocity[c].x);
    EXPECT_DOUBLE_EQ(0.0, field.pressure[c]);
  }
}

TEST(FlowField, RejectsUnusableInitialisation) {
  EXPECT_TRUE(FlowField::uniform(0, FreestreamConditions{}, 1.0).hasError());
  EXPECT_TRUE(FlowField::uniform(10, FreestreamConditions{}, 0.0).hasError());
  EXPECT_TRUE(FlowField::uniform(10, FreestreamConditions{.speed = -1.0}, 1.0).hasError());
}

TEST(FlowField, DetectsInconsistentArrays) {
  FlowField field;
  field.resize(8);
  field.pressure.pop_back();
  EXPECT_FALSE(field.isConsistent());
}

// ---------------------------------------------------------------------------
// Time and residuals
// ---------------------------------------------------------------------------

TEST(TimeState, AdvancesTimeAndCountsIterations) {
  TimeState clock;
  EXPECT_EQ(0, clock.iteration);

  clock.advance(0.01);
  clock.advance(0.02);

  EXPECT_NEAR(0.03, clock.time, 1e-15);
  EXPECT_NEAR(0.02, clock.timeStep, 1e-15);
  EXPECT_EQ(2, clock.iteration);

  clock.reset();
  EXPECT_EQ(0, clock.iteration);
  EXPECT_DOUBLE_EQ(0.0, clock.time);
}

TEST(ResidualSet, WorstIsTheLargestMagnitude) {
  const ResidualSet residuals{.continuity = 1e-4, .momentumX = -3e-3, .momentumY = 2e-5};
  EXPECT_DOUBLE_EQ(3e-3, residuals.worst());
}

TEST(ResidualHistory, KeepsRecentEntriesAndCountsEvictions) {
  ResidualHistory history{4};

  for (int i = 0; i < 7; ++i) {
    history.record(i, ResidualSet{.continuity = static_cast<double>(i)});
  }

  ASSERT_EQ(4u, history.size());
  EXPECT_EQ(3, history.entries().front().iteration);
  EXPECT_EQ(6, history.entries().back().iteration);
  EXPECT_EQ(3u, history.droppedCount());
  EXPECT_NEAR(6.0, history.latest().continuity, 1e-15);
}

TEST(ResidualHistory, IsEmptyBeforeAnythingIsRecorded) {
  const ResidualHistory history;
  EXPECT_TRUE(history.empty());
  EXPECT_NEAR(0.0, history.latest().worst(), 1e-18);
}

TEST(ResidualHistory, ClearResetsContentsAndDropCount) {
  ResidualHistory history{2};
  for (int i = 0; i < 5; ++i) {
    history.record(i, ResidualSet{});
  }
  ASSERT_GT(history.droppedCount(), 0u);

  history.clear();
  EXPECT_TRUE(history.empty());
  EXPECT_EQ(0u, history.droppedCount());
}

}  // namespace
