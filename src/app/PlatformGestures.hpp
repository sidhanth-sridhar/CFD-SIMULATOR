// PlatformGestures.hpp - trackpad gestures that GLFW does not expose.
//
// GLFW reports scroll wheels and two-finger scrolling, but has no notion of a
// pinch. On macOS a pinch arrives as an AppKit magnification gesture, which
// never becomes a GLFW event, so reaching it means going to the native window
// behind the GLFW handle.
//
// The interface is deliberately tiny and platform-neutral: install the
// handlers once at start-up, then drain the accumulated magnification once per
// frame. On platforms with no implementation both calls are harmless no-ops
// and the application simply keeps its scroll-wheel zoom.

#pragma once

struct GLFWwindow;

namespace cfd::app::platform {

/// Attach native gesture recognition to `window`, where the platform offers
/// it. Safe to call with a null window; safe to call once per application.
///
/// Returns true if pinch handling is actually active, so the caller can say so
/// rather than leaving the user guessing why a gesture does nothing.
bool installGestureHandlers(GLFWwindow* window);

/// Magnification accumulated since the previous call, and reset to zero.
///
/// Follows the platform convention: the value is a *relative* change, so the
/// zoom factor it asks for is (1 + value). Positive means the fingers moved
/// apart, i.e. zoom in. Returns zero where gestures are unsupported.
[[nodiscard]] double consumePinchMagnification();

}  // namespace cfd::app::platform
