// Fallback for platforms with no gesture support wired up.
//
// Compiled instead of PlatformGestures.mm everywhere except macOS. The
// application keeps its scroll-wheel zoom and simply never sees a pinch.

#include "PlatformGestures.hpp"

namespace cfd::app::platform {

bool installGestureHandlers(GLFWwindow* /*window*/) { return false; }

double consumePinchMagnification() { return 0.0; }

}  // namespace cfd::app::platform
