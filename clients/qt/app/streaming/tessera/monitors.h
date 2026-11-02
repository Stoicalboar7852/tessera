// Tessera multi-display: the client's monitors (docs/phase-3-design.md, component 3).
#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include "displayplan.h"

namespace tessera {

struct ClientMonitor {
    QByteArray key;          // 16 bytes, stable for a physical monitor (a hash of the OS's monitor ID)
    QString name;
    int sdlIndex = -1;
    Rect desktop;            // SDL display bounds: pixels on Windows, points on macOS
    int nativeWidth = 0;     // native pixels (below the notch on MacBooks)
    int nativeHeight = 0;
    int desktopWidth = 0;    // the size of the scaled desktop, which "Desktop size" streams
    int desktopHeight = 0;
    double scale = 1.0;      // native pixels per desktop unit
    Refresh refresh {60, 1};
    bool hdr = false;        // HDR is on (Windows) or available (macOS) for this monitor
    bool primary = false;    // the OS primary monitor (the one with the menu bar on macOS)
};

// Every monitor SDL knows, in SDL display order. Initializes SDL's video subsystem for the call, so on macOS it must
// run on the main thread.
QVector<ClientMonitor> enumerateMonitors();

// Fills in the OS's view of the monitor at `monitor.desktop` (key, name, exact refresh rate, scale, HDR, primary).
// Returns false if no OS monitor has those bounds; the caller then keeps what SDL reported.
// Implemented in monitors_win.cpp and monitors_mac.mm.
bool describeMonitor(ClientMonitor& monitor);

// The 16-byte key for an OS monitor ID
QByteArray monitorKey(const QByteArray& osId);

}
