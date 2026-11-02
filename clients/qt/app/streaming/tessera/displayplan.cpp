#include "displayplan.h"

#include <algorithm>
#include <cmath>

namespace tessera {

namespace {

// Desktop coordinates of neighbouring monitors may differ by a unit after scaling
constexpr int kTouchTolerance = 2;

bool overlaps(int a0, int a1, int b0, int b1)
{
    return std::min(a1, b1) > std::max(a0, b0);
}

// Places `m` next to the already placed `n` if they touch; returns false if they don't.
bool placeNextTo(const LayoutInput& m, const LayoutInput& n, const Point& nPos, Point& out)
{
    const Rect& a = n.desktop;
    const Rect& b = m.desktop;
    const double sx = a.w > 0 ? static_cast<double>(n.pixelWidth) / a.w : 1.0;
    const double sy = a.h > 0 ? static_cast<double>(n.pixelHeight) / a.h : 1.0;

    if (overlaps(a.y, a.y + a.h, b.y, b.y + b.h)) {
        const int y = nPos.y + static_cast<int>(std::lround((b.y - a.y) * sy));
        if (std::abs(b.x - (a.x + a.w)) <= kTouchTolerance) {
            out = {nPos.x + n.pixelWidth, y};
            return true;
        }
        if (std::abs(b.x + b.w - a.x) <= kTouchTolerance) {
            out = {nPos.x - m.pixelWidth, y};
            return true;
        }
    }
    if (overlaps(a.x, a.x + a.w, b.x, b.x + b.w)) {
        const int x = nPos.x + static_cast<int>(std::lround((b.x - a.x) * sx));
        if (std::abs(b.y - (a.y + a.h)) <= kTouchTolerance) {
            out = {x, nPos.y + n.pixelHeight};
            return true;
        }
        if (std::abs(b.y + b.h - a.y) <= kTouchTolerance) {
            out = {x, nPos.y - m.pixelHeight};
            return true;
        }
    }
    return false;
}

}

std::vector<Point> layoutInPixels(const std::vector<LayoutInput>& monitors, int origin)
{
    const size_t count = monitors.size();
    std::vector<Point> positions(count);
    std::vector<bool> placed(count, false);

    if (origin < 0 || static_cast<size_t>(origin) >= count) {
        return positions;
    }
    placed[origin] = true;

    for (size_t done = 1; done < count; done++) {
        bool found = false;

        // Breadth first from the origin: the first unplaced monitor touching a placed one
        for (size_t i = 0; i < count && !found; i++) {
            if (placed[i]) {
                continue;
            }
            for (size_t j = 0; j < count; j++) {
                if (placed[j] && placeNextTo(monitors[i], monitors[j], positions[j], positions[i])) {
                    placed[i] = true;
                    found = true;
                    break;
                }
            }
        }
        if (found) {
            continue;
        }

        // Nothing touches: the leftmost remaining monitor goes to the right of everything placed
        int right = 0;
        for (size_t j = 0; j < count; j++) {
            if (placed[j]) {
                right = std::max(right, positions[j].x + monitors[j].pixelWidth);
            }
        }
        size_t next = count;
        for (size_t i = 0; i < count; i++) {
            if (!placed[i] && (next == count || monitors[i].desktop.x < monitors[next].desktop.x)) {
                next = i;
            }
        }
        positions[next] = {right, 0};
        placed[next] = true;
    }
    return positions;
}

Refresh refreshFraction(double hz)
{
    if (!(hz > 0)) {
        return {60, 1};
    }

    const long whole = std::lround(hz);
    if (std::abs(hz - whole) <= 0.02) {
        return {static_cast<int>(whole), 1};
    }
    // NTSC-style rates: 59.94 = 60000/1001, 119.88 = 120000/1001, 23.976 = 24000/1001
    const long ntsc = std::lround(hz * 1001.0 / 1000.0);
    if (std::abs(hz - ntsc * 1000.0 / 1001.0) <= 0.02) {
        return {static_cast<int>(ntsc * 1000), 1001};
    }
    return {static_cast<int>(std::lround(hz * 1000.0)), 1000};
}

std::vector<int> streamOrder(const std::vector<Rect>& desktop, const std::vector<bool>& chosen,
                             int osPrimary, int moonlightMonitor, int maxStreams)
{
    std::vector<int> others;
    for (size_t i = 0; i < desktop.size() && i < chosen.size(); i++) {
        if (chosen[i]) {
            others.push_back(static_cast<int>(i));
        }
    }
    std::stable_sort(others.begin(), others.end(), [&desktop](int a, int b) {
        return desktop[a].x != desktop[b].x ? desktop[a].x < desktop[b].x : desktop[a].y < desktop[b].y;
    });
    if (others.empty() || maxStreams < 1) {
        return {};
    }

    auto isChosen = [&](int index) {
        return std::find(others.begin(), others.end(), index) != others.end();
    };
    int first = others.front();
    if (isChosen(osPrimary)) {
        first = osPrimary;
    }
    else if (isChosen(moonlightMonitor)) {
        first = moonlightMonitor;
    }

    std::vector<int> order {first};
    for (int index : others) {
        if (index != first && static_cast<int>(order.size()) < maxStreams) {
            order.push_back(index);
        }
    }
    return order;
}

}
