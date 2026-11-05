// Tessera multi-display: which monitors a session streams, and their settings (docs/phase-3-design.md, component 4).
#pragma once

#include <QHash>
#include <QObject>
#include <QPoint>
#include <QStringList>
#include <QVariantList>
#include <QVector>

#include <Tessera.h>

#include "monitors.h"

#include <functional>

namespace tessera {

// How one monitor streams. The defaults stream it as it is: native resolution and refresh rate, automatic bitrate.
struct MonitorSettings {
    enum Resolution { Native = 0, DesktopSize = 1, Lines = 2 };

    Resolution resolution = Native;
    int lines = 0;         // with Lines: the height to stream, at the monitor's aspect ratio
    int fps = 0;           // 0: the monitor's refresh rate
    int bitrateKbps = 0;   // 0: automatic (upstream's default bitrate for the display's size and frame rate)

    bool operator==(const MonitorSettings& other) const;
};

// Stored with QSettings next to upstream's preferences
struct DisplaySettings {
    enum Mode { ThisDisplay = 0, AllDisplays = 1, ChosenDisplays = 2 };

    Mode mode = ThisDisplay;
    QStringList chosenKeys;   // ClientMonitor::key in hex

    // By ClientMonitor::key in hex. A monitor without an entry gets `defaultResolution` and the other defaults.
    QHash<QString, MonitorSettings> monitors;
    // The resolution for monitors without their own settings (the single "Resolution of each display" setting
    // of the first test builds)
    MonitorSettings::Resolution defaultResolution = MonitorSettings::Native;

    MonitorSettings monitorSettings(const QString& key) const;

    static DisplaySettings load();
    void save() const;
};

// Upstream's bitrate rules, passed in so the planner doesn't depend on StreamingPreferences
struct BitrateRules {
    // Upstream's default bitrate in kbps for one stream of this size and frame rate. May be empty in tests: the
    // displays then get no bitrate of their own and the total is split by pixel rate.
    std::function<int(int width, int height, int fps)> defaultKbps;
    // The settings slider's maximum: a chosen bitrate is capped at it
    int maximumKbps = 150000;
};

// What a monitor streams at with its settings
struct StreamMode {
    int width = 0;
    int height = 0;
    Refresh refresh {60, 1};
};

StreamMode resolveMode(const ClientMonitor& monitor, const MonitorSettings& settings);

// The bitrate for one display: its chosen bitrate (at least 500 kbps, at most rules.maximumKbps), else the default
int resolveBitrateKbps(const StreamMode& mode, const MonitorSettings& settings, const BitrateRules& rules);

struct DisplayPlan {
    // In stream order: index 0 is stream 0. Empty when the session streams one display the upstream way.
    QVector<ClientMonitor> monitors;
    QVector<TESSERA_DISPLAY> displays;
    QStringList warnings;
};

// moonlightScreen is the top-left corner of the screen showing Moonlight's window (as Session::getWindowDimensions()
// matches it). hdr is the user's HDR setting. Each display's bitrateKbps is filled in from `rules`.
DisplayPlan planDisplays(const DisplaySettings& settings, const QVector<ClientMonitor>& monitors,
                         QPoint moonlightScreen, bool hdr, const BitrateRules& rules);

// Writes the plan to the log
void logPlan(const DisplayPlan& plan);

// The session's total video bitrate: the sum of the displays' bitrates
int planBitrateKbps(const DisplayPlan& plan);

// The "Displays" settings section (gui/TesseraDisplays.qml), registered as the QML singleton TesseraDisplaySettings.
// Changes are saved immediately.
class DisplaySettingsModel : public QObject
{
    Q_OBJECT

    Q_PROPERTY(int mode READ mode WRITE setMode NOTIFY modeChanged)
    Q_PROPERTY(QVariantList monitors READ monitors NOTIFY monitorsChanged)
    // Streaming several displays: the mode isn't "This display" and at least two monitors would stream. The upstream
    // resolution, frame rate and bitrate settings then don't apply.
    Q_PROPERTY(bool active READ active NOTIFY monitorsChanged)
    Q_PROPERTY(int streamCount READ streamCount NOTIFY monitorsChanged)
    Q_PROPERTY(int totalBitrateKbps READ totalBitrateKbps NOTIFY monitorsChanged)
    Q_PROPERTY(int maximumBitrateKbps READ maximumBitrateKbps NOTIFY monitorsChanged)

public:
    // `rules` is asked again whenever the monitors are listed or a setting changes
    explicit DisplaySettingsModel(const std::function<BitrateRules()>& rules = {}, QObject* parent = nullptr);

    int mode() const;
    void setMode(int mode);
    QVariantList monitors() const;
    bool active() const;
    int streamCount() const;
    int totalBitrateKbps() const;
    int maximumBitrateKbps() const;

    // Lists the monitors again. Each entry is a map:
    //   key, name, primary, hdr, chosen, streamed (it would stream with the current mode),
    //   x, y, w, h (its place on the desktop, for the layout picture),
    //   width, height, refresh (what it streams at), sizeOptions/sizeIndex, frameRateOptions/frameRateIndex,
    //   autoBitrate, bitrateKbps (the bitrate it streams at), defaultBitrateKbps
    Q_INVOKABLE void refreshMonitors();
    Q_INVOKABLE void setChosen(const QString& key, bool chosen);
    // index into the monitor's sizeOptions / frameRateOptions
    Q_INVOKABLE void setSize(const QString& key, int index);
    Q_INVOKABLE void setFrameRate(const QString& key, int index);
    // 0 for automatic
    Q_INVOKABLE void setBitrate(const QString& key, int kbps);

    // As refreshMonitors() with a given list (tests)
    void setMonitorList(const QVector<ClientMonitor>& monitors);
    const DisplaySettings& settings() const;

signals:
    void modeChanged();
    void monitorsChanged();

private:
    struct SizeOption {
        MonitorSettings::Resolution resolution;
        int lines;
    };
    struct MonitorOptions {
        QVector<SizeOption> sizes;
        QVector<int> frameRates;   // 0 first: the monitor's refresh rate
    };

    int monitorIndex(const QString& key) const;
    void updateMonitor(const QString& key, const MonitorSettings& settings);
    void rebuild();

    std::function<BitrateRules()> m_RulesSource;
    BitrateRules m_Rules;
    DisplaySettings m_Settings;
    QVector<ClientMonitor> m_ClientMonitors;
    QVector<MonitorOptions> m_Options;
    QVariantList m_Monitors;
    int m_StreamCount = 0;
    int m_TotalBitrateKbps = 0;
};

}
