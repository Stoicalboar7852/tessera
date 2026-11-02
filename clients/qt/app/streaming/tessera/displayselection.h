// Tessera multi-display: which monitors a session streams, and their settings (docs/phase-3-design.md, component 4).
#pragma once

#include <QObject>
#include <QPoint>
#include <QStringList>
#include <QVariantList>
#include <QVector>

#include <Tessera.h>

#include "monitors.h"

#include <functional>

namespace tessera {

// Stored with QSettings next to upstream's preferences
struct DisplaySettings {
    enum Mode { ThisDisplay = 0, AllDisplays = 1, ChosenDisplays = 2 };
    enum Resolution { Native = 0, DesktopSize = 1 };

    Mode mode = ThisDisplay;
    QStringList chosenKeys;   // ClientMonitor::key in hex
    Resolution resolution = Native;

    static DisplaySettings load();
    void save() const;
};

struct DisplayPlan {
    // In stream order: index 0 is stream 0. Empty when the session streams one display the upstream way.
    QVector<ClientMonitor> monitors;
    QVector<TESSERA_DISPLAY> displays;
    QStringList warnings;
};

// moonlightScreen is the top-left corner of the screen showing Moonlight's window (as Session::getWindowDimensions()
// matches it). hdr is the user's HDR setting.
DisplayPlan planDisplays(const DisplaySettings& settings, const QVector<ClientMonitor>& monitors,
                         QPoint moonlightScreen, bool hdr);

// The session's total video bitrate: with automatic bitrate, the sum of defaultKbps(width, height, fps) for each display
// (upstream's default bitrate), capped at maximumKbps (the settings slider's maximum); otherwise the user's bitrate.
int planBitrateKbps(const DisplayPlan& plan, bool autoAdjust, int userKbps, int maximumKbps,
                    const std::function<int(int, int, int)>& defaultKbps);

// The "Displays" settings section (gui/TesseraDisplays.qml), registered as the QML singleton TesseraDisplaySettings.
// Changes are saved immediately.
class DisplaySettingsModel : public QObject
{
    Q_OBJECT

    Q_PROPERTY(int mode READ mode WRITE setMode NOTIFY modeChanged)
    Q_PROPERTY(int resolution READ resolution WRITE setResolution NOTIFY resolutionChanged)
    Q_PROPERTY(QVariantList monitors READ monitors NOTIFY monitorsChanged)

public:
    explicit DisplaySettingsModel(QObject* parent = nullptr);

    int mode() const;
    void setMode(int mode);
    int resolution() const;
    void setResolution(int resolution);
    QVariantList monitors() const;

    // Lists the monitors again: [{key, name, nativeSize, desktopSize, refresh, hdr, primary, chosen}]
    Q_INVOKABLE void refreshMonitors();
    Q_INVOKABLE void setChosen(const QString& key, bool chosen);

signals:
    void modeChanged();
    void resolutionChanged();
    void monitorsChanged();

private:
    DisplaySettings m_Settings;
    QVariantList m_Monitors;
};

}
