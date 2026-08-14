// Vec2.hpp - the 2D point/vector type shared across the project.
//
// One type for both points and directions. Mathematically those are different
// things (a point is an element of an affine space, a vector of the associated
// vector space), and some codebases separate them to stop you adding two
// points together. Here they share a type: the distinction buys little in 2D,
// and every geometry, mesh and field routine would otherwise need conversions
// at each boundary.
//
// Lives in cfd_core so that geometry and the GUI can exchange coordinates
// without the geometry module depending on anything graphical.
//
// Double precision throughout. Airfoil coordinates span roughly [0, 1] while
// boundary-layer detail sits at 1e-5 of chord, and later the solver will
// subtract nearly-equal quantities to form gradients - float's ~7 significant
// digits would not survive that.

#pragma once

#include <cmath>

namespace cfd {

struct Vec2 {
  double x{0.0};
  double y{0.0};

  friend constexpr bool operator==(const Vec2&, const Vec2&) = default;

  constexpr Vec2& operator+=(const Vec2& rhs) noexcept {
    x += rhs.x;
    y += rhs.y;
    return *this;
  }
  constexpr Vec2& operator-=(const Vec2& rhs) noexcept {
    x -= rhs.x;
    y -= rhs.y;
    return *this;
  }
  constexpr Vec2& operator*=(double s) noexcept {
    x *= s;
    y *= s;
    return *this;
  }
};

[[nodiscard]] constexpr Vec2 operator+(Vec2 a, const Vec2& b) noexcept { return a += b; }
[[nodiscard]] constexpr Vec2 operator-(Vec2 a, const Vec2& b) noexcept { return a -= b; }
[[nodiscard]] constexpr Vec2 operator*(Vec2 v, double s) noexcept { return v *= s; }
[[nodiscard]] constexpr Vec2 operator*(double s, Vec2 v) noexcept { return v *= s; }
[[nodiscard]] constexpr Vec2 operator-(Vec2 v) noexcept { return Vec2{-v.x, -v.y}; }

[[nodiscard]] constexpr double dot(const Vec2& a, const Vec2& b) noexcept {
  return a.x * b.x + a.y * b.y;
}

/// z component of the 3D cross product of two vectors in the xy plane. Its
/// sign tells you which side of `a` the vector `b` lies on, which is how
/// polygon orientation and point-in-triangle tests are decided.
[[nodiscard]] constexpr double cross(const Vec2& a, const Vec2& b) noexcept {
  return a.x * b.y - a.y * b.x;
}

[[nodiscard]] inline double length(const Vec2& v) noexcept {
  return std::hypot(v.x, v.y);  // avoids overflow that sqrt(x*x + y*y) can hit
}

[[nodiscard]] constexpr double lengthSquared(const Vec2& v) noexcept { return dot(v, v); }

[[nodiscard]] inline double distance(const Vec2& a, const Vec2& b) noexcept {
  return length(b - a);
}

}  // namespace cfd
