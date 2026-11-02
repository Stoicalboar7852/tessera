#include "monitors.h"

#import <AppKit/AppKit.h>
#include <ApplicationServices/ApplicationServices.h>

#include <cmath>

namespace tessera {

bool describeMonitor(ClientMonitor& monitor)
{
    @autoreleasepool {
        for (NSScreen* screen in [NSScreen screens]) {
            CGDirectDisplayID displayId = [[[screen deviceDescription] objectForKey:@"NSScreenNumber"] unsignedIntValue];

            // SDL's display bounds are CoreGraphics' global display coordinates, in points
            CGRect bounds = CGDisplayBounds(displayId);
            if (std::lround(bounds.origin.x) != monitor.desktop.x || std::lround(bounds.origin.y) != monitor.desktop.y ||
                    std::lround(bounds.size.width) != monitor.desktop.w ||
                    std::lround(bounds.size.height) != monitor.desktop.h) {
                continue;
            }

            CFUUIDRef uuid = CGDisplayCreateUUIDFromDisplayID(displayId);
            if (uuid != nullptr) {
                CFStringRef uuidString = CFUUIDCreateString(kCFAllocatorDefault, uuid);
                monitor.key = monitorKey(QString::fromNSString((__bridge NSString*)uuidString).toLower().toUtf8());
                CFRelease(uuidString);
                CFRelease(uuid);
            }
            else {
                monitor.key = monitorKey(QStringLiteral("%1-%2-%3")
                                             .arg(CGDisplayVendorNumber(displayId))
                                             .arg(CGDisplayModelNumber(displayId))
                                             .arg(CGDisplaySerialNumber(displayId)).toUtf8());
            }

            monitor.name = QString::fromNSString([screen localizedName]);
            monitor.primary = CGDisplayIsMain(displayId);

            // ProMotion and some external displays report 0 for the mode's rate
            double hz = 0;
            CGDisplayModeRef mode = CGDisplayCopyDisplayMode(displayId);
            if (mode != nullptr) {
                hz = CGDisplayModeGetRefreshRate(mode);
                CGDisplayModeRelease(mode);
            }
            if (!(hz > 0)) {
                hz = static_cast<double>([screen maximumFramesPerSecond]);
            }
            if (hz > 0) {
                monitor.refresh = refreshFraction(hz);
            }

            if (bounds.size.width > 0) {
                monitor.scale = monitor.nativeWidth / bounds.size.width;
            }
            monitor.hdr = [screen maximumPotentialExtendedDynamicRangeColorComponentValue] > 1.0;
            return true;
        }
    }
    return false;
}

}
