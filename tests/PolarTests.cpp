// Tests for the polar: the list of angles a sweep visits, the table it fills,
// and the CSV it writes.
//
// The sweep itself is a state machine over frames in the application layer and
// needs a window to run, so what is tested here is everything that does not:
// the arithmetic that decides which angles get solved, the summary a filled
// table reports, and the exact text that lands in the file. Those are the parts
// where a mistake is silent - a sweep that quietly skips its last angle, or a
// CSV whose columns do not line up with its header - and they are all pure
// functions of their inputs.

#include "cfd/post/Polar.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using cfd::post::Polar;
using cfd::post::PolarPoint;
using cfd::post::sweepAngles;
using cfd::post::toCsv;

std::vector<std::string> splitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    lines.push_back(line);
  }
  return lines;
}

std::vector<std::string> splitFields(const std::string& line) {
  std::vector<std::string> fields;
  std::string field;
  std::istringstream stream(line);
  while (std::getline(stream, field, ',')) {
    fields.push_back(field);
  }
  // getline drops a trailing empty field, and the separation columns are
  // legitimately empty, so put it back when the line ends on a comma.
  if (!line.empty() && line.back() == ',') {
    fields.emplace_back();
  }
  return fields;
}

// ---------------------------------------------------------------------------
// Which angles get solved
// ---------------------------------------------------------------------------

// The example from the specification, and the case that has to be exactly
// right: 0 to 18 in steps of 2 is ten angles, ending on 18 and not on 16.
TEST(Polar, SweepCoversTheRequestedAnglesInclusive) {
  const auto angles = sweepAngles(0.0, 18.0, 2.0);
  ASSERT_TRUE(angles);
  ASSERT_EQ(angles.value().size(), 10u);
  EXPECT_DOUBLE_EQ(angles.value().front(), 0.0);
  EXPECT_DOUBLE_EQ(angles.value().back(), 18.0);
  for (std::size_t i = 0; i < angles.value().size(); ++i) {
    EXPECT_DOUBLE_EQ(angles.value()[i], 2.0 * static_cast<double>(i));
  }
}

// Angles are computed rather than accumulated, so a long sweep must land
// exactly on its end rather than a rounding error short of it.
TEST(Polar, SweepDoesNotDriftOverManySteps) {
  const auto angles = sweepAngles(0.0, 18.0, 0.1);
  ASSERT_TRUE(angles);
  EXPECT_EQ(angles.value().size(), 181u);
  EXPECT_DOUBLE_EQ(angles.value().back(), 18.0);
}

// A step that does not divide the range stops at the last whole step rather
// than overshooting past the end the user asked for.
TEST(Polar, SweepStopsAtTheLastWholeStep) {
  const auto angles = sweepAngles(0.0, 5.0, 2.0);
  ASSERT_TRUE(angles);
  ASSERT_EQ(angles.value().size(), 3u);
  EXPECT_DOUBLE_EQ(angles.value().back(), 4.0);
}

TEST(Polar, SweepHandlesNegativeStartAngles) {
  const auto angles = sweepAngles(-6.0, 6.0, 3.0);
  ASSERT_TRUE(angles);
  ASSERT_EQ(angles.value().size(), 5u);
  EXPECT_DOUBLE_EQ(angles.value().front(), -6.0);
  EXPECT_DOUBLE_EQ(angles.value().back(), 6.0);
}

TEST(Polar, SweepOfASinglePointIsAllowed) {
  const auto angles = sweepAngles(4.0, 4.0, 1.0);
  ASSERT_TRUE(angles);
  ASSERT_EQ(angles.value().size(), 1u);
  EXPECT_DOUBLE_EQ(angles.value().front(), 4.0);
}

TEST(Polar, SweepRejectsANonPositiveStep) {
  EXPECT_FALSE(sweepAngles(0.0, 10.0, 0.0));
  EXPECT_FALSE(sweepAngles(0.0, 10.0, -1.0));
}

// A descending sweep is the same sweep walked backwards. It exists so that
// sweeping down can be compared with sweeping up, which is the only way to
// detect hysteresis introduced by continuing each point from the last.
TEST(Polar, SweepCanRunDownwards) {
  const auto angles = sweepAngles(18.0, 0.0, 2.0);
  ASSERT_TRUE(angles);
  ASSERT_EQ(angles.value().size(), 10u);
  EXPECT_DOUBLE_EQ(angles.value().front(), 18.0);
  EXPECT_DOUBLE_EQ(angles.value().back(), 0.0);
  for (std::size_t i = 1; i < angles.value().size(); ++i) {
    EXPECT_LT(angles.value()[i], angles.value()[i - 1]);
  }
}

// Up and down over the same range must visit exactly the same angles, or the
// two sweeps would not be comparable and the hysteresis check would be
// measuring the sweep rather than the flow.
TEST(Polar, SweepingDownVisitsTheSameAnglesAsSweepingUp) {
  const auto up = sweepAngles(0.0, 18.0, 2.0);
  const auto down = sweepAngles(18.0, 0.0, 2.0);
  ASSERT_TRUE(up);
  ASSERT_TRUE(down);
  ASSERT_EQ(up.value().size(), down.value().size());

  const std::size_t n = up.value().size();
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_DOUBLE_EQ(up.value()[i], down.value()[n - 1 - i]);
  }
}

// Every point is a full Navier-Stokes solve, so a mistyped step is minutes or
// hours of work. It is refused rather than started.
TEST(Polar, SweepRefusesAnUnreasonableNumberOfPoints) {
  EXPECT_FALSE(sweepAngles(0.0, 20.0, 0.001));
}

// ---------------------------------------------------------------------------
// The filled table
// ---------------------------------------------------------------------------

Polar samplePolar() {
  Polar polar;
  polar.section = "NACA 0012";
  polar.meshResolution = "Coarse";
  polar.reynoldsNumber = 500.0;
  polar.machEquivalentSpeed = 50.0;
  polar.chord = 1.0;

  const double lift[] = {0.0, 0.111, 0.222, 0.425};
  const double drag[] = {0.186, 0.188, 0.195, 0.220};
  for (int i = 0; i < 4; ++i) {
    PolarPoint point;
    point.angleOfAttackDeg = 2.0 * i;
    point.liftCoefficient = lift[i];
    point.dragCoefficient = drag[i];
    point.liftToDrag = lift[i] / drag[i];
    point.converged = true;
    point.iterations = 1000 + i;
    polar.points.push_back(point);
  }
  return polar;
}

TEST(Polar, BestLiftToDragIsTheLargestConvergedPoint) {
  const Polar polar = samplePolar();
  const int best = polar.bestLiftToDragIndex();
  ASSERT_GE(best, 0);
  EXPECT_EQ(best, 3);
  EXPECT_TRUE(polar.allConverged());
}

// An unconverged point is not a design point, however good its numbers look.
TEST(Polar, BestLiftToDragIgnoresUnconvergedPoints) {
  Polar polar = samplePolar();
  polar.points[3].converged = false;
  polar.points[3].liftToDrag = 99.0;

  EXPECT_FALSE(polar.allConverged());
  const int best = polar.bestLiftToDragIndex();
  ASSERT_GE(best, 0);
  EXPECT_EQ(best, 2);
}

TEST(Polar, AnEmptyPolarHasNoBestPointAndIsNotConverged) {
  const Polar polar;
  EXPECT_TRUE(polar.empty());
  EXPECT_EQ(polar.bestLiftToDragIndex(), -1);
  EXPECT_FALSE(polar.allConverged());
}

// ---------------------------------------------------------------------------
// Sweep sequencing
// ---------------------------------------------------------------------------

using cfd::post::nextSweepAction;
using cfd::post::SweepAction;
using cfd::post::SweepObservation;
using cfd::post::SweepPhase;

constexpr int kBudget = 240;

// The one that matters, and the bug this function exists to prevent. Right
// after an angle is requested the solver has not started yet, which looks
// identical to having finished. Recording here would attribute the *previous*
// angle's forces to this one - a polar that is silently shifted by one point.
TEST(Polar, ASweepDoesNotRecordBeforeTheSolverHasStarted) {
  SweepObservation observation;
  observation.solverRunning = false;
  observation.framesWaiting = 0;

  EXPECT_EQ(nextSweepAction(SweepPhase::Starting, observation, kBudget),
            SweepAction::Wait);
}

TEST(Polar, ASweepStartsWatchingOnceTheSolverIsRunning) {
  SweepObservation observation;
  observation.solverRunning = true;

  EXPECT_EQ(nextSweepAction(SweepPhase::Starting, observation, kBudget),
            SweepAction::BeginSolving);
}

TEST(Polar, ASweepWaitsWhileTheSolverIsStillWorking) {
  SweepObservation observation;
  observation.solverRunning = true;

  EXPECT_EQ(nextSweepAction(SweepPhase::Solving, observation, kBudget),
            SweepAction::Wait);
}

TEST(Polar, ASweepRecordsWhenTheSolverStops) {
  SweepObservation observation;
  observation.solverRunning = false;

  EXPECT_EQ(nextSweepAction(SweepPhase::Solving, observation, kBudget),
            SweepAction::RecordPoint);
}

// Waiting forever looks exactly like working, which is the worst failure for an
// operation that legitimately takes minutes.
TEST(Polar, ASweepGivesUpIfTheSolverNeverStarts) {
  SweepObservation observation;
  observation.solverRunning = false;
  observation.framesWaiting = kBudget;
  EXPECT_EQ(nextSweepAction(SweepPhase::Starting, observation, kBudget),
            SweepAction::Wait);

  observation.framesWaiting = kBudget + 1;
  EXPECT_EQ(nextSweepAction(SweepPhase::Starting, observation, kBudget),
            SweepAction::AbortNotStarted);
}

// A solver that failed to build never reports itself as running, so the failure
// has to be read before the timeout - otherwise a real error is reported as a
// timeout 240 frames later.
TEST(Polar, ASweepReportsASolverFailureImmediately) {
  SweepObservation observation;
  observation.solverRunning = false;
  observation.solverFailed = true;
  observation.framesWaiting = 0;

  EXPECT_EQ(nextSweepAction(SweepPhase::Starting, observation, kBudget),
            SweepAction::AbortFailed);
}

TEST(Polar, AnIdleSweepDoesNothing) {
  SweepObservation observation;
  observation.solverRunning = false;
  EXPECT_EQ(nextSweepAction(SweepPhase::Idle, observation, kBudget), SweepAction::Wait);

  observation.solverRunning = true;
  EXPECT_EQ(nextSweepAction(SweepPhase::Idle, observation, kBudget), SweepAction::Wait);
}

// ---------------------------------------------------------------------------
// The CSV
// ---------------------------------------------------------------------------

TEST(Polar, CsvHasOneHeaderAndOneRowPerPoint) {
  const Polar polar = samplePolar();
  const std::vector<std::string> lines = splitLines(toCsv(polar));

  std::size_t comments = 0;
  for (const std::string& line : lines) {
    if (!line.empty() && line.front() == '#') {
      ++comments;
    }
  }
  ASSERT_GT(comments, 0u);
  // Comment block, then exactly one header, then one row per point.
  ASSERT_EQ(lines.size(), comments + 1 + polar.points.size());
  EXPECT_EQ(lines[comments].rfind("alpha_deg,cl,cd", 0), 0u);
}

// Every row must have exactly as many fields as the header names, or a reader
// silently pairs values with the wrong columns.
TEST(Polar, EveryCsvRowMatchesTheHeaderWidth) {
  Polar polar = samplePolar();
  // One attached point and one separated, so both branches of the optional
  // separation columns are exercised.
  polar.points[0].upperSeparation = -1.0;
  polar.points[0].lowerSeparation = -1.0;
  polar.points[3].upperSeparation = 0.6923;
  polar.points[3].lowerSeparation = -1.0;

  const std::vector<std::string> lines = splitLines(toCsv(polar));
  std::size_t headerIndex = 0;
  while (headerIndex < lines.size() && lines[headerIndex].front() == '#') {
    ++headerIndex;
  }
  ASSERT_LT(headerIndex, lines.size());

  const std::size_t columns = splitFields(lines[headerIndex]).size();
  ASSERT_EQ(columns, 12u);
  for (std::size_t i = headerIndex + 1; i < lines.size(); ++i) {
    EXPECT_EQ(splitFields(lines[i]).size(), columns) << "row " << i << ": " << lines[i];
  }
}

TEST(Polar, CsvCarriesTheConditionsAndTheValues) {
  const Polar polar = samplePolar();
  const std::string csv = toCsv(polar);

  // The conditions, without which the coefficients mean nothing.
  EXPECT_NE(csv.find("# section,NACA 0012"), std::string::npos);
  EXPECT_NE(csv.find("# reynolds,500"), std::string::npos);
  EXPECT_NE(csv.find("# mesh,Coarse"), std::string::npos);

  // And the values themselves, to the precision written.
  EXPECT_NE(csv.find("6.000000,0.42500000"), std::string::npos);
}

TEST(Polar, CsvMarksUnconvergedPoints) {
  Polar polar = samplePolar();
  polar.points[2].converged = false;

  const std::vector<std::string> lines = splitLines(toCsv(polar));
  std::size_t marked = 0;
  for (const std::string& line : lines) {
    if (!line.empty() && line.front() != '#' && line.find(",no,") != std::string::npos) {
      ++marked;
    }
  }
  EXPECT_EQ(marked, 1u);
}

TEST(Polar, WritesAndReadsBackTheSameText) {
  const Polar polar = samplePolar();
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "cfd_polar_roundtrip.csv";

  ASSERT_TRUE(cfd::post::writeCsv(polar, path.string()));

  std::ifstream in(path, std::ios::binary);
  ASSERT_TRUE(in);
  const std::string read{std::istreambuf_iterator<char>(in),
                         std::istreambuf_iterator<char>()};
  in.close();
  std::filesystem::remove(path);

  EXPECT_EQ(read, toCsv(polar));
}

TEST(Polar, WriteRejectsAnEmptyPath) {
  EXPECT_FALSE(cfd::post::writeCsv(samplePolar(), ""));
}

}  // namespace
