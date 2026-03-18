#import <AppKit/AppKit.h>
#include "platform_activate.h"

namespace EYEQ {

void ActivateApp() {
  @autoreleasepool {
    // Activate the application to bring it to the foreground.
    // On macOS 14+, NSApplicationActivateIgnoringOtherApps is deprecated and
    // activateWithOptions:0 is the recommended replacement. The system controls
    // whether the activation is granted based on context.
    [[NSRunningApplication currentApplication] activateWithOptions:0];
  }
}

} // namespace EYEQ
