// Tests for the world <-> screen transform used by the viewport.
//
// Camera2D is header-only and pulls in no GUI dependency, so this runs in the
// ordinary headless test binary. The properties checked here are the ones a
// user notices immediately when they break: the y axis pointing the wrong way,
// zoom drifting away from the cursor, or a "fit" that crops the geometry.

#include "cfd/app/Camera2D.hpp"

#include <gtest/gtest.h>

#include <cmath>

namespace {

using cfd::app::Camera2D;
using cfd::app::Vec2;

constexpr double kTol = 1e-9;

Camera2D makeCamera(double width = 800.0, double height = 600.0,
                    double pixelsPerUnit = 200.0) {
  Camera2D camera;
  camera.setViewportSize(width, height);
  camera.setPixelsPerUnit(pixelsPerUnit);
  camera.setCenter(Vec2{0.0, 0.0});
  return camera;
}

TEST(Camera2D, CameraCentreLandsAtViewportCentre) {
  Camera2D camera = makeCamera();
  camera.setCenter(Vec2{3.0, -1.0});

  const Vec2 screen = camera.worldToScreen(Vec2{3.0, -1.0});

  EXPECT_NEAR(400.0, screen.x, kTol);
  EXPECT_NEAR(300.0, screen.y, kTol);
}

// The single most common orientation bug: world y up, screen y down.
TEST(Camera2D, WorldYPointsUpOnScreen) {
  const Camera2D camera = makeCamera();

  const Vec2 low = camera.worldToScreen(Vec2{0.0, -1.0});
  const Vec2 high = camera.worldToScreen(Vec2{0.0, 1.0});

  EXPECT_LT(high.y, low.y) << "increasing world y must move up the screen";
  EXPECT_NEAR(low.x, high.x, kTol) << "a pure y change must not shift x";
}

TEST(Camera2D, ScreenToWorldInvertsWorldToScreen) {
  Camera2D camera = makeCamera(1024.0, 768.0, 137.5);
  camera.setCenter(Vec2{-2.5, 0.75});

  constexpr Vec2 kProbes[] = {
      {0.0, 0.0}, {1.0, 0.5}, {-3.25, 2.125}, {12.0, -7.5},
  };

  for (const Vec2& world : kProbes) {
    const Vec2 roundTrip = camera.screenToWorld(camera.worldToScreen(world));
    EXPECT_NEAR(world.x, roundTrip.x, 1e-9);
    EXPECT_NEAR(world.y, roundTrip.y, 1e-9);
  }
}

TEST(Camera2D, ScaleIsUniformSoShapesAreNotDistorted) {
  // A non-square viewport must not stretch the world; a circle has to stay a
  // circle, or every angle and curvature read off the screen would be wrong.
  const Camera2D camera = makeCamera(1600.0, 400.0, 200.0);

  const Vec2 origin = camera.worldToScreen(Vec2{0.0, 0.0});
  const Vec2 alongX = camera.worldToScreen(Vec2{1.0, 0.0});
  const Vec2 alongY = camera.worldToScreen(Vec2{0.0, 1.0});

  EXPECT_NEAR(std::abs(alongX.x - origin.x), std::abs(alongY.y - origin.y), kTol);
}

// Zooming must keep whatever is under the cursor under the cursor. Without
// this, inspecting a boundary layer means constantly re-panning.
TEST(Camera2D, ZoomKeepsThePointUnderTheCursorFixed) {
  Camera2D camera = makeCamera();
  const Vec2 anchor{620.0, 145.0};
  const Vec2 worldBefore = camera.screenToWorld(anchor);

  camera.zoomAboutScreenPoint(2.0, anchor);
  const Vec2 worldAfterZoomIn = camera.screenToWorld(anchor);

  EXPECT_NEAR(worldBefore.x, worldAfterZoomIn.x, 1e-9);
  EXPECT_NEAR(worldBefore.y, worldAfterZoomIn.y, 1e-9);

  camera.zoomAboutScreenPoint(0.25, anchor);
  const Vec2 worldAfterZoomOut = camera.screenToWorld(anchor);

  EXPECT_NEAR(worldBefore.x, worldAfterZoomOut.x, 1e-9);
  EXPECT_NEAR(worldBefore.y, worldAfterZoomOut.y, 1e-9);
}

TEST(Camera2D, ZoomChangesScaleByTheRequestedFactor) {
  Camera2D camera = makeCamera();
  const double before = camera.pixelsPerUnit();

  camera.zoomAboutScreenPoint(1.5, Vec2{400.0, 300.0});

  EXPECT_NEAR(before * 1.5, camera.pixelsPerUnit(), 1e-9);
}

TEST(Camera2D, ZoomIsClampedToUsableRange) {
  Camera2D camera = makeCamera();

  for (int i = 0; i < 400; ++i) {
    camera.zoomAboutScreenPoint(4.0, Vec2{400.0, 300.0});
  }
  EXPECT_LE(camera.pixelsPerUnit(), Camera2D::kMaxPixelsPerUnit);
  EXPECT_TRUE(std::isfinite(camera.pixelsPerUnit()));

  for (int i = 0; i < 400; ++i) {
    camera.zoomAboutScreenPoint(0.25, Vec2{400.0, 300.0});
  }
  EXPECT_GE(camera.pixelsPerUnit(), Camera2D::kMinPixelsPerUnit);
  EXPECT_TRUE(std::isfinite(camera.pixelsPerUnit()));
}

TEST(Camera2D, InvalidZoomFactorsAreIgnored) {
  Camera2D camera = makeCamera();
  const double before = camera.pixelsPerUnit();

  camera.zoomAboutScreenPoint(0.0, Vec2{10.0, 10.0});
  camera.zoomAboutScreenPoint(-2.0, Vec2{10.0, 10.0});
  camera.zoomAboutScreenPoint(std::nan(""), Vec2{10.0, 10.0});

  EXPECT_DOUBLE_EQ(before, camera.pixelsPerUnit());
}

// Dragging right must carry the contents right, i.e. the world point under the
// mouse stays under the mouse.
TEST(Camera2D, PanMovesContentWithTheMouse) {
  Camera2D camera = makeCamera();
  const Vec2 start{400.0, 300.0};
  const Vec2 worldUnderMouse = camera.screenToWorld(start);

  const Vec2 drag{50.0, -20.0};
  camera.panByScreenDelta(drag);

  const Vec2 nowAt = camera.worldToScreen(worldUnderMouse);
  EXPECT_NEAR(start.x + drag.x, nowAt.x, 1e-9);
  EXPECT_NEAR(start.y + drag.y, nowAt.y, 1e-9);
}

TEST(Camera2D, FrameBoxCentresAndFitsTheWholeBox) {
  Camera2D camera = makeCamera(1000.0, 500.0, 1.0);

  const Vec2 lower{0.0, -0.1};
  const Vec2 upper{1.0, 0.1};
  camera.frameBox(lower, upper, 0.05);

  EXPECT_NEAR(0.5, camera.center().x, kTol);
  EXPECT_NEAR(0.0, camera.center().y, kTol);

  // Every corner has to be inside the viewport rectangle.
  const Vec2 corners[] = {
      {lower.x, lower.y}, {upper.x, lower.y}, {lower.x, upper.y}, {upper.x, upper.y}};
  for (const Vec2& corner : corners) {
    const Vec2 screen = camera.worldToScreen(corner);
    EXPECT_GE(screen.x, 0.0);
    EXPECT_LE(screen.x, 1000.0);
    EXPECT_GE(screen.y, 0.0);
    EXPECT_LE(screen.y, 500.0);
  }
}

TEST(Camera2D, FrameBoxUsesTheMoreRestrictiveAxis) {
  // A wide, flat box in a square viewport must be limited by its width, not
  // its height - otherwise the leading and trailing edges fall off-screen.
  Camera2D camera = makeCamera(600.0, 600.0, 1.0);
  camera.frameBox(Vec2{0.0, -0.05}, Vec2{1.0, 0.05}, 0.0);

  EXPECT_NEAR(600.0, camera.pixelsPerUnit(), 1e-9);
}

TEST(Camera2D, DegenerateViewportDoesNotProduceNonFiniteValues) {
  Camera2D camera;
  camera.setViewportSize(0.0, 0.0);  // a fully collapsed panel

  const Vec2 world = camera.screenToWorld(Vec2{0.0, 0.0});
  EXPECT_TRUE(std::isfinite(world.x));
  EXPECT_TRUE(std::isfinite(world.y));
  EXPECT_GT(camera.viewportWidth(), 0.0);
  EXPECT_GT(camera.viewportHeight(), 0.0);
}

TEST(Camera2D, VisibleBoundsMatchTheViewportCorners) {
  Camera2D camera = makeCamera(800.0, 600.0, 100.0);
  camera.setCenter(Vec2{1.0, 2.0});

  const auto [minWorld, maxWorld] = camera.visibleBounds();

  EXPECT_NEAR(1.0 - 4.0, minWorld.x, kTol);  // 800 px / 2 / 100 px per m
  EXPECT_NEAR(1.0 + 4.0, maxWorld.x, kTol);
  EXPECT_NEAR(2.0 - 3.0, minWorld.y, kTol);  // 600 px / 2 / 100 px per m
  EXPECT_NEAR(2.0 + 3.0, maxWorld.y, kTol);
  EXPECT_LT(minWorld.x, maxWorld.x);
  EXPECT_LT(minWorld.y, maxWorld.y);
}

}  // namespace
