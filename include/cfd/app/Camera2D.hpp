// Camera2D.hpp - the mapping between physical space and the screen.
//
// Two coordinate systems meet in the viewport, and confusing them is the
// classic source of "why is my airfoil upside down":
//
//   World space   metres. x increases to the right, y increases UP.
//                 This is the frame the geometry, mesh and flow field live
//                 in, and matches how aerodynamic figures are drawn.
//
//   Screen space  pixels. x increases to the right, y increases DOWN,
//                 because that is how every raster display and GUI toolkit
//                 addresses its framebuffer.
//
// So the transform is a uniform scale plus a translation, with the y axis
// negated. Uniform - the same pixels-per-metre on both axes - matters
// physically: a non-uniform scale would stretch the airfoil and make angles
// and curvature read wrong, and later it would misrepresent the shape of
// vortices and separation bubbles.
//
// Header-only and free of any GUI dependency, so the transform can be unit
// tested without a window.

#pragma once

#include <algorithm>
#include <cmath>
#include <utility>

namespace cfd::app {

/// Minimal 2D point. Deliberately not a general linear-algebra type: the
/// solver will bring its own vector types with different requirements.
struct Vec2 {
  double x{0.0};
  double y{0.0};

  friend constexpr bool operator==(const Vec2&, const Vec2&) = default;
};

/// An orthographic 2D view: a world-space point held at the viewport centre,
/// and a zoom expressed as pixels per world unit.
class Camera2D {
 public:
  /// Zoom limits. At 1e6 px/m a metre spans a million pixels, which is far
  /// past the point where single-precision rendering coordinates break down;
  /// at 1e-4 the whole visible world is 10 km across.
  static constexpr double kMinPixelsPerUnit = 1e-4;
  static constexpr double kMaxPixelsPerUnit = 1e6;

  /// Viewport size in pixels. Clamped away from zero so that a collapsed
  /// panel cannot produce a division by zero in screenToWorld.
  void setViewportSize(double width, double height) noexcept {
    width_ = std::max(width, 1.0);
    height_ = std::max(height, 1.0);
  }

  [[nodiscard]] double viewportWidth() const noexcept { return width_; }
  [[nodiscard]] double viewportHeight() const noexcept { return height_; }

  void setCenter(Vec2 world) noexcept { center_ = world; }
  [[nodiscard]] Vec2 center() const noexcept { return center_; }

  void setPixelsPerUnit(double value) noexcept {
    if (!std::isfinite(value) || value <= 0.0) {
      return;
    }
    pixelsPerUnit_ = std::clamp(value, kMinPixelsPerUnit, kMaxPixelsPerUnit);
  }
  [[nodiscard]] double pixelsPerUnit() const noexcept { return pixelsPerUnit_; }

  /// Screen pixel for a world point. Screen coordinates are relative to the
  /// top-left corner of the viewport rectangle, not the OS window.
  [[nodiscard]] Vec2 worldToScreen(Vec2 world) const noexcept {
    return Vec2{
        width_ * 0.5 + (world.x - center_.x) * pixelsPerUnit_,
        height_ * 0.5 - (world.y - center_.y) * pixelsPerUnit_,  // y flips here
    };
  }

  /// Exact inverse of worldToScreen.
  [[nodiscard]] Vec2 screenToWorld(Vec2 screen) const noexcept {
    return Vec2{
        center_.x + (screen.x - width_ * 0.5) / pixelsPerUnit_,
        center_.y - (screen.y - height_ * 0.5) / pixelsPerUnit_,
    };
  }

  /// Drag the view by a mouse delta measured in pixels. Moving the mouse
  /// right must move the *contents* right, which means the camera centre
  /// moves left - hence the subtraction.
  void panByScreenDelta(Vec2 pixelDelta) noexcept {
    center_.x -= pixelDelta.x / pixelsPerUnit_;
    center_.y += pixelDelta.y / pixelsPerUnit_;  // screen y is inverted
  }

  /// Scroll-wheel zoom that keeps the world point under the cursor pinned in
  /// place. Zooming about the viewport centre instead makes a tool feel
  /// unusable as soon as you are inspecting a boundary layer off to one side.
  void zoomAboutScreenPoint(double factor, Vec2 screenAnchor) noexcept {
    if (!std::isfinite(factor) || factor <= 0.0) {
      return;
    }
    const Vec2 anchorWorld = screenToWorld(screenAnchor);
    setPixelsPerUnit(pixelsPerUnit_ * factor);

    // Re-derive the centre so that anchorWorld still lands on screenAnchor.
    center_.x = anchorWorld.x - (screenAnchor.x - width_ * 0.5) / pixelsPerUnit_;
    center_.y = anchorWorld.y + (screenAnchor.y - height_ * 0.5) / pixelsPerUnit_;
  }

  /// Fit an axis-aligned world box to the viewport, leaving `margin` of slack
  /// as a fraction of the viewport on each side. The smaller of the two
  /// required scales wins, so the whole box fits and the aspect ratio is
  /// preserved. This is the "zoom to fit" a user presses after loading
  /// geometry.
  void frameBox(Vec2 minWorld, Vec2 maxWorld, double margin = 0.08) noexcept {
    const double spanX = std::abs(maxWorld.x - minWorld.x);
    const double spanY = std::abs(maxWorld.y - minWorld.y);

    center_ = Vec2{0.5 * (minWorld.x + maxWorld.x), 0.5 * (minWorld.y + maxWorld.y)};

    const double usable = std::clamp(1.0 - 2.0 * margin, 0.05, 1.0);
    const double scaleX = (spanX > 0.0) ? (width_ * usable) / spanX : kMaxPixelsPerUnit;
    const double scaleY = (spanY > 0.0) ? (height_ * usable) / spanY : kMaxPixelsPerUnit;

    setPixelsPerUnit(std::min(scaleX, scaleY));
  }

  /// World-space extent currently visible, as (min, max).
  [[nodiscard]] std::pair<Vec2, Vec2> visibleBounds() const noexcept {
    const Vec2 topLeft = screenToWorld(Vec2{0.0, 0.0});
    const Vec2 bottomRight = screenToWorld(Vec2{width_, height_});
    return {Vec2{topLeft.x, bottomRight.y}, Vec2{bottomRight.x, topLeft.y}};
  }

 private:
  Vec2 center_{0.0, 0.0};
  double pixelsPerUnit_{600.0};
  double width_{1.0};
  double height_{1.0};
};

}  // namespace cfd::app
