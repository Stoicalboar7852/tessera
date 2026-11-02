// Tessera multi-display: planning helpers without Qt or SDL, so they can be unit-tested on their own
// (clients/qt/tests/tessera). See docs/phase-3-design.md.
#pragma once

#include <vector>

namespace tessera {

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

struct Point {
    int x = 0;
    int y = 0;
};

struct LayoutInput {
    Rect desktop;     // the monitor in the OS desktop coordinate space (pixels on Windows, points on macOS)
    int pixelWidth;   // the size that will be streamed
    int pixelHeight;
};

// Positions in pixels for the host's layout. Monitor `origin` is at (0, 0). Every other monitor sits next to a
// monitor it touches on the client's desktop, on the same side and with the same alignment (offsets scaled by that
// neighbour's pixels per desktop unit), so monitors with different scale factors still touch. A monitor that
// touches none of the others goes to the right of them, top-aligned with the origin.
std::vector<Point> layoutInPixels(const std::vector<LayoutInput>& monitors, int origin);

struct Refresh {
    int numerator;
    int denominator;
};

// An exact fraction for a refresh rate the OS reports as a floating-point number: n/1 or n*1000/1001 when within
// 0.02 Hz of one, else rounded to millihertz. Non-positive rates give 60/1.
Refresh refreshFraction(double hz);

// The monitors to stream, by index into `desktop`, in stream order: stream 0 first (the OS primary if it is chosen,
// else the monitor showing Moonlight if it is chosen, else the leftmost chosen one), then the other chosen monitors
// left to right (top to bottom for equal x). At most `maxStreams` entries.
std::vector<int> streamOrder(const std::vector<Rect>& desktop, const std::vector<bool>& chosen,
                             int osPrimary, int moonlightMonitor, int maxStreams);

}
