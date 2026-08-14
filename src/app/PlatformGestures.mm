// macOS implementation of trackpad pinch handling.
//
// A pinch is an NSEvent magnification gesture delivered to the window's content
// view. GLFW creates and owns that view, so rather than subclass it we attach
// an NSMagnificationGestureRecognizer, which AppKit will drive without any
// cooperation from GLFW.
//
// The recognizer fires on the main thread from inside AppKit's event pump -
// which is what glfwWaitEventsTimeout() is running - so the accumulated value
// is only ever touched from one thread and needs no synchronisation. It also
// means a pinch wakes the event loop by itself, exactly like a scroll.

#import <Cocoa/Cocoa.h>

// GLFW_INCLUDE_NONE normally comes from the build; define it only if it did
// not, so this file still compiles standalone.
#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include "PlatformGestures.hpp"

namespace {

/// Magnification seen since the application last drained it.
double gPendingMagnification = 0.0;

}  // namespace

/// Target for the gesture recognizer.
///
/// NSMagnificationGestureRecognizer reports `magnification` as a running total
/// for the gesture in progress, not a per-callback increment, so we difference
/// it ourselves and reset at the start of each new gesture.
@interface CfdPinchTarget : NSObject
@property(nonatomic) double lastMagnification;
- (void)handlePinch:(NSMagnificationGestureRecognizer *)recognizer;
@end

@implementation CfdPinchTarget

- (void)handlePinch:(NSMagnificationGestureRecognizer *)recognizer {
  if (recognizer.state == NSGestureRecognizerStateBegan) {
    self.lastMagnification = 0.0;
  }

  const double total = recognizer.magnification;
  gPendingMagnification += total - self.lastMagnification;
  self.lastMagnification = total;

  if (recognizer.state == NSGestureRecognizerStateEnded ||
      recognizer.state == NSGestureRecognizerStateCancelled) {
    self.lastMagnification = 0.0;
  }
}

@end

namespace cfd::app::platform {

bool installGestureHandlers(GLFWwindow* window) {
  if (window == nullptr) {
    return false;
  }
  NSWindow* nativeWindow = glfwGetCocoaWindow(window);
  if (nativeWindow == nil) {
    return false;
  }
  NSView* view = [nativeWindow contentView];
  if (view == nil) {
    return false;
  }

  // The target is deliberately kept alive for the lifetime of the process; the
  // recognizer holds only a weak reference to it, so letting it go out of
  // scope would leave the gesture firing at a dead object.
  static CfdPinchTarget* target = nil;
  if (target == nil) {
    target = [[CfdPinchTarget alloc] init];
  }

  NSMagnificationGestureRecognizer* recognizer =
      [[NSMagnificationGestureRecognizer alloc] initWithTarget:target
                                                        action:@selector(handlePinch:)];
  [view addGestureRecognizer:recognizer];
  return true;
}

double consumePinchMagnification() {
  const double pending = gPendingMagnification;
  gPendingMagnification = 0.0;
  return pending;
}

}  // namespace cfd::app::platform
