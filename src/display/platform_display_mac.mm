#import <AppKit/AppKit.h>
#include <dlfcn.h>
#include <SDL3/SDL.h>
#include <spdlog/fmt/fmt.h>

#include "platform_display.h"

namespace EYEQ {

namespace {

// Calls a zero-argument getter through KVC after checking that it exists, so a
// changed private interface yields nil instead of an Objective-C exception.
id GetIfResponds(id object, NSString *getter) {
  if (!object || ![object respondsToSelector:NSSelectorFromString(getter)]) {
    return nil;
  }
  return [object valueForKey:getter];
}

// The active display preset lives in the private MonitorPanel framework (used by
// System Settings). The interface is undocumented and may change with macOS
// updates; every failure is reported through preset_error.
void QueryActivePreset(CGDirectDisplayID display_id, DisplayEDRInfo &info) {
  static void *handle = dlopen("/System/Library/PrivateFrameworks/MonitorPanel.framework/MonitorPanel", RTLD_LAZY);
  if (!handle) {
    info.preset_error = "MonitorPanel framework not available";
    return;
  }
  Class mgr_class = NSClassFromString(@"MPDisplayMgr");
  if (!mgr_class) {
    info.preset_error = "MPDisplayMgr class not found";
    return;
  }
  id mgr = [[mgr_class alloc] init];
#if !__has_feature(objc_arc)
  [mgr autorelease];
#endif
  id displays = GetIfResponds(mgr, @"displays");
  if (![displays isKindOfClass:[NSArray class]]) {
    info.preset_error = "MPDisplayMgr.displays unavailable";
    return;
  }

  for (id display in (NSArray *)displays) {
    id number = GetIfResponds(display, @"displayID");
    if (![number isKindOfClass:[NSNumber class]] || [number unsignedIntValue] != display_id) {
      continue;
    }
    id has_presets = GetIfResponds(display, @"hasPresets");
    if (![has_presets isKindOfClass:[NSNumber class]] || ![has_presets boolValue]) {
      info.preset_error = "display has no presets";
      return;
    }
    id dict = GetIfResponds(GetIfResponds(display, @"activePreset"), @"presetDictionary");
    if (![dict isKindOfClass:[NSDictionary class]]) {
      info.preset_error = "active preset dictionary unavailable";
      return;
    }
    id name = dict[@"PresetName"];
    id peak = dict[@"PresetMaxHDRLuminance"];
    id sdr = dict[@"PresetMaxSDRLuminance"];
    if (![name isKindOfClass:[NSString class]] || ![peak isKindOfClass:[NSNumber class]] ||
        ![sdr isKindOfClass:[NSNumber class]]) {
      info.preset_error = "active preset lacks name or luminance keys";
      return;
    }
    info.preset_name = [name UTF8String];
    info.preset_peak_nits = [peak floatValue];
    info.preset_sdr_white_nits = [sdr floatValue];
    info.preset_valid = true;
    return;
  }
  info.preset_error = fmt::format("display {} not found in MonitorPanel", display_id);
}

} // namespace

DisplayEDRInfo QueryDisplayEDR(SDL_Window *window) {
  DisplayEDRInfo info;
  @autoreleasepool {
    NSWindow *nswindow = (__bridge NSWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                                     SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
    NSScreen *screen = nswindow ? [nswindow screen] : nil;
    if (!screen) {
      info.error = "window has no NSScreen";
      return info;
    }
    info.valid = true;
    info.display_name = [[screen localizedName] UTF8String];
    if (@available(macOS 10.15, *)) {
      info.potential_headroom = static_cast<float>([screen maximumPotentialExtendedDynamicRangeColorComponentValue]);
    }
    if (@available(macOS 12.0, *)) {
      info.reference_headroom = static_cast<float>([screen maximumReferenceExtendedDynamicRangeColorComponentValue]);
    }
    NSNumber *number = [screen deviceDescription][@"NSScreenNumber"];
    QueryActivePreset(static_cast<CGDirectDisplayID>([number unsignedIntValue]), info);
  }
  return info;
}

} // namespace EYEQ
