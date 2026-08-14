// Tests for the Result/Error value-based error channel.

#include "cfd/core/Error.hpp"

#include <gtest/gtest.h>

#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace {

using cfd::Error;
using cfd::ErrorCode;
using cfd::Result;
using cfd::Status;

// A function shaped the way real Phase 1+ code will be: returns a value on
// success, a described failure otherwise.
Result<double> chordFromString(const std::string& text) {
  try {
    const double value = std::stod(text);
    if (value <= 0.0) {
      return Error{ErrorCode::InvalidArgument,
                   "chord must be positive, got " + text};
    }
    return value;
  } catch (const std::exception&) {
    return Error{ErrorCode::InvalidArgument, "not a number: " + text};
  }
}

TEST(Result, CarriesValueOnSuccess) {
  const Result<double> result = chordFromString("1.5");

  ASSERT_TRUE(result.hasValue());
  EXPECT_TRUE(static_cast<bool>(result));
  EXPECT_FALSE(result.hasError());
  EXPECT_DOUBLE_EQ(1.5, result.value());
}

TEST(Result, CarriesErrorOnFailure) {
  const Result<double> result = chordFromString("-2.0");

  ASSERT_TRUE(result.hasError());
  EXPECT_FALSE(static_cast<bool>(result));
  EXPECT_EQ(ErrorCode::InvalidArgument, result.error().code());
  EXPECT_NE(std::string::npos, result.error().message().find("must be positive"));
}

// The whole point of the type: an unchecked read is loud, not silently wrong.
// A solver that reads a garbage value after a failed mesh load produces
// plausible-looking nonsense, which is far worse than a crash.
TEST(Result, ReadingValueOfAnErrorThrows) {
  const Result<double> result = chordFromString("banana");

  EXPECT_THROW((void)result.value(), cfd::BadResultAccess);
}

TEST(Result, ReadingErrorOfAValueThrows) {
  const Result<double> result = chordFromString("0.75");

  EXPECT_THROW((void)result.error(), cfd::BadResultAccess);
}

TEST(Result, ValueOrSubstitutesFallbackOnError) {
  EXPECT_DOUBLE_EQ(2.0, chordFromString("2.0").valueOr(99.0));
  EXPECT_DOUBLE_EQ(99.0, chordFromString("nonsense").valueOr(99.0));
}

// Mesh and field objects will be move-only or expensive to copy, so Result
// has to move them out rather than force a copy.
TEST(Result, MovesMoveOnlyPayloadOut) {
  Result<std::unique_ptr<int>> result{std::make_unique<int>(42)};

  ASSERT_TRUE(result.hasValue());
  std::unique_ptr<int> owned = std::move(result).value();

  ASSERT_NE(nullptr, owned);
  EXPECT_EQ(42, *owned);
}

TEST(Status, DistinguishesSuccessFromFailure) {
  const Status ok = Status::ok();
  EXPECT_TRUE(ok.hasValue());
  EXPECT_TRUE(static_cast<bool>(ok));

  const Status failed = Error{ErrorCode::IoFailure, "disk on fire"};
  EXPECT_TRUE(failed.hasError());
  EXPECT_EQ(ErrorCode::IoFailure, failed.error().code());
}

TEST(Error, FormatIncludesCodeMessageAndOrigin) {
  const Error error{ErrorCode::NumericalFailure, "residual became NaN"};
  const int line_of_construction = __LINE__ - 1;

  const std::string text = error.format();

  EXPECT_NE(std::string::npos, text.find("NumericalFailure"));
  EXPECT_NE(std::string::npos, text.find("residual became NaN"));
  // Origin is captured at the construction site, not inside Error's own code.
  EXPECT_NE(std::string::npos, text.find("ErrorTests.cpp"));
  EXPECT_NE(std::string::npos, text.find(std::to_string(line_of_construction)));
}

TEST(ErrorCode, EveryCodeHasADistinctName) {
  constexpr ErrorCode kCodes[] = {
      ErrorCode::InvalidArgument, ErrorCode::OutOfRange,
      ErrorCode::NotFound,        ErrorCode::IoFailure,
      ErrorCode::NotImplemented,  ErrorCode::InitializationFailure,
      ErrorCode::NumericalFailure, ErrorCode::Internal,
  };

  std::set<std::string_view> names;
  for (const ErrorCode code : kCodes) {
    const std::string_view name = cfd::toString(code);
    EXPECT_FALSE(name.empty());
    EXPECT_NE("UnknownErrorCode", name) << "toString is missing a case";
    names.insert(name);
  }

  // A copy-paste slip in the switch would map two codes to one name and make
  // logs ambiguous.
  EXPECT_EQ(std::size(kCodes), names.size());
}

}  // namespace
