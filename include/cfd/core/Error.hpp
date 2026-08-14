// Error.hpp - explicit, value-based error reporting.
//
// Why not exceptions?
// ------------------
// A CFD run has two very different classes of failure:
//
//   * Programming errors (index out of bounds, broken invariant). These are
//     bugs; the right response is to abort loudly. Assertions handle them.
//
//   * Expected, recoverable failures: a mesh file that will not parse, a
//     geometry that self-intersects, a solve that diverges at iteration 4000.
//     These are *data* about the run, not exceptional control flow, and the
//     caller almost always wants to inspect and report them.
//
// The second class is what this header is for. `Result<T>` makes the failure
// path part of the function signature, so it cannot be forgotten: the type is
// marked [[nodiscard]], and reading the value without checking throws rather
// than returning garbage. Exceptions would also work, but they hide the
// failure path from the signature and are awkward to propagate out of the
// tight numeric loops that dominate a solver.

#pragma once

#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace cfd {

/// Broad failure categories. Kept deliberately small: a category earns a slot
/// only when a caller would plausibly branch on it.
enum class ErrorCode {
  InvalidArgument,        ///< Caller supplied a value outside the accepted domain.
  OutOfRange,             ///< Index or coordinate outside a valid extent.
  NotFound,               ///< A named resource does not exist.
  IoFailure,              ///< File or stream operation failed.
  NotImplemented,         ///< Reached a code path scheduled for a later phase.
  InitializationFailure,  ///< A subsystem (window, GL context, ...) failed to start.
  NumericalFailure,       ///< Divergence, NaN/Inf, singular system.
  Internal,               ///< Broken invariant that was not fatal enough to abort.
};

/// Stable, human-readable name for a code. Never allocates.
[[nodiscard]] std::string_view toString(ErrorCode code) noexcept;

/// A failure: what went wrong, described in prose, plus where it originated.
///
/// The source location is captured automatically via a defaulted argument, so
/// `return Error{ErrorCode::IoFailure, "cannot open " + path};` records the
/// call site with no extra syntax at the point of use.
class Error {
 public:
  Error(ErrorCode code, std::string message,
        std::source_location origin = std::source_location::current())
      : code_(code), message_(std::move(message)), origin_(origin) {}

  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }
  [[nodiscard]] const std::source_location& origin() const noexcept { return origin_; }

  /// "InvalidArgument: chord must be positive (Airfoil.cpp:42)"
  [[nodiscard]] std::string format() const;

 private:
  ErrorCode code_;
  std::string message_;
  std::source_location origin_;
};

/// Thrown when value() is called on a Result that holds an error, or error()
/// on one that holds a value. Reaching this is a bug in the caller.
class BadResultAccess : public std::logic_error {
 public:
  explicit BadResultAccess(const std::string& what) : std::logic_error(what) {}
};

/// Either a `T` or an `Error`.
///
/// Both constructors are intentionally implicit so that a function returning
/// `Result<Mesh>` can `return mesh;` or `return Error{...};` directly.
template <typename T>
class [[nodiscard]] Result {
  static_assert(!std::is_same_v<std::remove_cvref_t<T>, Error>,
                "Result<Error> is ambiguous; use Result<void> for a pure status.");
  static_assert(!std::is_reference_v<T>, "Result cannot hold a reference.");

 public:
  using value_type = T;

  Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
  Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}

  [[nodiscard]] bool hasValue() const noexcept { return storage_.index() == 0; }
  [[nodiscard]] bool hasError() const noexcept { return !hasValue(); }
  explicit operator bool() const noexcept { return hasValue(); }

  [[nodiscard]] const T& value() const& {
    requireValue();
    return std::get<0>(storage_);
  }
  [[nodiscard]] T& value() & {
    requireValue();
    return std::get<0>(storage_);
  }
  /// Move the contained value out of an rvalue Result.
  [[nodiscard]] T&& value() && {
    requireValue();
    return std::get<0>(std::move(storage_));
  }

  [[nodiscard]] const Error& error() const& {
    requireError();
    return std::get<1>(storage_);
  }

  /// Value if present, otherwise `fallback`. Never throws for the caller.
  template <typename U>
  [[nodiscard]] T valueOr(U&& fallback) const& {
    return hasValue() ? std::get<0>(storage_) : static_cast<T>(std::forward<U>(fallback));
  }

 private:
  void requireValue() const {
    if (!hasValue()) {
      throw BadResultAccess("Result::value() called on an error: " +
                            std::get<1>(storage_).format());
    }
  }
  void requireError() const {
    if (hasValue()) {
      throw BadResultAccess("Result::error() called on a Result holding a value");
    }
  }

  std::variant<T, Error> storage_;
};

/// Specialisation for operations that either succeed or fail but produce no
/// value - "did this converge?", "did this write?".
template <>
class [[nodiscard]] Result<void> {
 public:
  using value_type = void;

  Result() = default;  ///< success
  Result(Error error) : error_(std::move(error)) {}

  [[nodiscard]] static Result ok() noexcept { return Result{}; }

  [[nodiscard]] bool hasValue() const noexcept { return !error_.has_value(); }
  [[nodiscard]] bool hasError() const noexcept { return error_.has_value(); }
  explicit operator bool() const noexcept { return hasValue(); }

  [[nodiscard]] const Error& error() const& {
    if (!error_.has_value()) {
      throw BadResultAccess("Status::error() called on a successful status");
    }
    return *error_;
  }

 private:
  std::optional<Error> error_;
};

/// Readable spelling of Result<void> at call sites.
using Status = Result<void>;

}  // namespace cfd
