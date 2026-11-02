#include "monitors.h"

#include "streaming/streamutils.h"

#include <QCryptographicHash>

#include <SDL.h>

#include <cmath>

namespace tessera {

namespace {

int even(double value)
{
    return static_cast<int>(std::lround(value)) & ~1;
}

}

QByteArray monitorKey(const QByteArray& osId)
{
    return QCryptographicHash::hash(osId, QCryptographicHash::Sha256).left(16);
}

#if !defined(Q_OS_WIN32) && !defined(Q_OS_DARWIN)
bool describeMonitor(ClientMonitor&)
{
    return false;
}
#endif

QVector<ClientMonitor> enumerateMonitors()
{
    QVector<ClientMonitor> monitors;

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_VIDEO) failed: %s",
                     SDL_GetError());
        return monitors;
    }

    for (int i = 0; i < SDL_GetNumVideoDisplays(); i++) {
        SDL_Rect bounds;
        SDL_DisplayMode mode;
        SDL_Rect safeArea;
        if (SDL_GetDisplayBounds(i, &bounds) != 0 || !StreamUtils::getNativeDesktopMode(i, &mode, &safeArea) ||
                safeArea.w <= 0 || safeArea.h <= 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Tessera: skipping display %d (no bounds or native mode)", i);
            continue;
        }

        ClientMonitor monitor;
        monitor.sdlIndex = i;
        monitor.desktop = {bounds.x, bounds.y, bounds.w, bounds.h};
        monitor.nativeWidth = safeArea.w;
        monitor.nativeHeight = safeArea.h;
        monitor.refresh = refreshFraction(mode.refresh_rate);
        monitor.primary = bounds.x == 0 && bounds.y == 0;
        const char* name = SDL_GetDisplayName(i);
        monitor.name = name != nullptr ? QString::fromUtf8(name) : QStringLiteral("Display %1").arg(i + 1);

        if (!describeMonitor(monitor)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Tessera: no OS monitor at (%d,%d) %dx%d; using SDL's description of display %d",
                        bounds.x, bounds.y, bounds.w, bounds.h, i);
            monitor.key = monitorKey(monitor.name.toUtf8() + '@' + QByteArray::number(bounds.x) + ',' +
                                     QByteArray::number(bounds.y));
        }

        if (!(monitor.scale >= 1.0)) {
            monitor.scale = 1.0;
        }
        monitor.desktopWidth = even(monitor.nativeWidth / monitor.scale);
        monitor.desktopHeight = even(monitor.nativeHeight / monitor.scale);

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Tessera: display %d '%s' key %s at (%d,%d) %dx%d: native %dx%d, desktop %dx%d, %d/%d Hz%s%s",
                    i, qPrintable(monitor.name), monitor.key.toHex().constData(),
                    bounds.x, bounds.y, bounds.w, bounds.h, monitor.nativeWidth, monitor.nativeHeight,
                    monitor.desktopWidth, monitor.desktopHeight,
                    monitor.refresh.numerator, monitor.refresh.denominator,
                    monitor.hdr ? ", HDR" : "", monitor.primary ? ", primary" : "");
        monitors.append(monitor);
    }

    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return monitors;
}

}
