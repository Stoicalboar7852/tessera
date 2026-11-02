#include "displayselection.h"

#include <QSettings>
#include <QVariantMap>

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#define SER_TESSERA_DISPLAYS "tessera/displays"
#define SER_TESSERA_CHOSEN "tessera/chosenDisplays"
#define SER_TESSERA_RESOLUTION "tessera/displayResolution"

namespace tessera {

namespace {

QString refreshText(const Refresh& refresh)
{
    if (refresh.denominator == 1) {
        return QStringLiteral("%1 Hz").arg(refresh.numerator);
    }
    return QStringLiteral("%1 Hz").arg(static_cast<double>(refresh.numerator) / refresh.denominator, 0, 'f', 2);
}

}

DisplaySettings DisplaySettings::load()
{
    QSettings settings;
    DisplaySettings result;

    const int mode = settings.value(SER_TESSERA_DISPLAYS, ThisDisplay).toInt();
    result.mode = mode == AllDisplays || mode == ChosenDisplays ? static_cast<Mode>(mode) : ThisDisplay;
    result.chosenKeys = settings.value(SER_TESSERA_CHOSEN).toStringList();
    result.resolution = settings.value(SER_TESSERA_RESOLUTION, Native).toInt() == DesktopSize ? DesktopSize : Native;
    return result;
}

void DisplaySettings::save() const
{
    QSettings settings;
    settings.setValue(SER_TESSERA_DISPLAYS, static_cast<int>(mode));
    settings.setValue(SER_TESSERA_CHOSEN, chosenKeys);
    settings.setValue(SER_TESSERA_RESOLUTION, static_cast<int>(resolution));
}

DisplayPlan planDisplays(const DisplaySettings& settings, const QVector<ClientMonitor>& monitors,
                         QPoint moonlightScreen, bool hdr)
{
    DisplayPlan plan;
    if (settings.mode == DisplaySettings::ThisDisplay) {
        return plan;
    }

    std::vector<Rect> desktop;
    std::vector<bool> chosen;
    int osPrimary = -1;
    int moonlightMonitor = -1;
    int chosenCount = 0;
    for (int i = 0; i < monitors.size(); i++) {
        const ClientMonitor& monitor = monitors[i];
        desktop.push_back(monitor.desktop);
        const bool isChosen = settings.mode == DisplaySettings::AllDisplays ||
                              settings.chosenKeys.contains(QString::fromLatin1(monitor.key.toHex()));
        chosen.push_back(isChosen);
        chosenCount += isChosen ? 1 : 0;
        if (monitor.primary) {
            osPrimary = i;
        }
        if (monitor.desktop.x == moonlightScreen.x() && monitor.desktop.y == moonlightScreen.y()) {
            moonlightMonitor = i;
        }
    }

    if (chosenCount < 2) {
        if (settings.mode == DisplaySettings::ChosenDisplays) {
            plan.warnings.append(QObject::tr("Fewer than two of the chosen displays are connected. Streaming this display only."));
        }
        return plan;
    }

    const std::vector<int> order = streamOrder(desktop, chosen, osPrimary, moonlightMonitor, TESSERA_MAX_STREAMS);
    if (chosenCount > static_cast<int>(order.size())) {
        plan.warnings.append(QObject::tr("Streaming %1 of the %2 chosen displays (the most a session can stream).")
                                 .arg(order.size()).arg(chosenCount));
    }

    std::vector<LayoutInput> layoutInputs;
    for (int index : order) {
        const ClientMonitor& monitor = monitors[index];
        LayoutInput input;
        input.desktop = monitor.desktop;
        input.pixelWidth = settings.resolution == DisplaySettings::DesktopSize ? monitor.desktopWidth : monitor.nativeWidth;
        input.pixelHeight = settings.resolution == DisplaySettings::DesktopSize ? monitor.desktopHeight : monitor.nativeHeight;
        layoutInputs.push_back(input);
    }
    const std::vector<Point> positions = layoutInPixels(layoutInputs, 0);

    for (size_t i = 0; i < order.size(); i++) {
        const ClientMonitor& monitor = monitors[order[i]];
        TESSERA_DISPLAY display = {};
        const int keyLength = std::min(static_cast<int>(monitor.key.size()), TESSERA_MONITOR_KEY_MAX_BYTES);
        memcpy(display.key, monitor.key.constData(), keyLength);
        display.keyLength = keyLength;
        display.width = layoutInputs[i].pixelWidth;
        display.height = layoutInputs[i].pixelHeight;
        display.refreshNumerator = monitor.refresh.numerator;
        display.refreshDenominator = monitor.refresh.denominator;
        display.x = positions[i].x;
        display.y = positions[i].y;

        // 96 DPI per desktop unit, so the host picks the scale factor the client uses
        const int dpi = static_cast<int>(std::lround(96.0 * monitor.scale * display.width / monitor.nativeWidth));
        display.dpi = std::clamp(dpi, TESSERA_MIN_DPI, TESSERA_MAX_DPI);
        display.hdr = hdr && monitor.hdr;

        plan.monitors.append(monitor);
        plan.displays.append(display);

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Tessera: stream %d is '%s' at %dx%d %d/%d Hz, layout (%d,%d), %d DPI%s",
                    static_cast<int>(i), qPrintable(monitor.name), display.width, display.height,
                    display.refreshNumerator, display.refreshDenominator, display.x, display.y, display.dpi,
                    display.hdr ? ", HDR" : "");
    }
    return plan;
}

int planBitrateKbps(const DisplayPlan& plan, bool autoAdjust, int userKbps, int maximumKbps,
                    const std::function<int(int, int, int)>& defaultKbps)
{
    if (!autoAdjust || plan.displays.isEmpty()) {
        return userKbps;
    }

    long long total = 0;
    for (const TESSERA_DISPLAY& display : plan.displays) {
        const int fps = static_cast<int>(std::lround(static_cast<double>(display.refreshNumerator) /
                                                     display.refreshDenominator));
        total += defaultKbps(display.width, display.height, fps);
    }
    return static_cast<int>(std::min<long long>(total, maximumKbps));
}

DisplaySettingsModel::DisplaySettingsModel(QObject* parent)
    : QObject(parent),
      m_Settings(DisplaySettings::load())
{
}

int DisplaySettingsModel::mode() const
{
    return m_Settings.mode;
}

void DisplaySettingsModel::setMode(int mode)
{
    if (mode != m_Settings.mode &&
            (mode == DisplaySettings::ThisDisplay || mode == DisplaySettings::AllDisplays ||
             mode == DisplaySettings::ChosenDisplays)) {
        m_Settings.mode = static_cast<DisplaySettings::Mode>(mode);
        m_Settings.save();
        emit modeChanged();
    }
}

int DisplaySettingsModel::resolution() const
{
    return m_Settings.resolution;
}

void DisplaySettingsModel::setResolution(int resolution)
{
    if (resolution != m_Settings.resolution &&
            (resolution == DisplaySettings::Native || resolution == DisplaySettings::DesktopSize)) {
        m_Settings.resolution = static_cast<DisplaySettings::Resolution>(resolution);
        m_Settings.save();
        emit resolutionChanged();
    }
}

QVariantList DisplaySettingsModel::monitors() const
{
    return m_Monitors;
}

void DisplaySettingsModel::refreshMonitors()
{
    m_Monitors.clear();
    for (const ClientMonitor& monitor : enumerateMonitors()) {
        const QString key = QString::fromLatin1(monitor.key.toHex());
        QVariantMap entry;
        entry.insert(QStringLiteral("key"), key);
        entry.insert(QStringLiteral("name"), monitor.name);
        entry.insert(QStringLiteral("nativeSize"), QStringLiteral("%1x%2").arg(monitor.nativeWidth).arg(monitor.nativeHeight));
        entry.insert(QStringLiteral("desktopSize"), QStringLiteral("%1x%2").arg(monitor.desktopWidth).arg(monitor.desktopHeight));
        entry.insert(QStringLiteral("refresh"), refreshText(monitor.refresh));
        entry.insert(QStringLiteral("hdr"), monitor.hdr);
        entry.insert(QStringLiteral("primary"), monitor.primary);
        entry.insert(QStringLiteral("chosen"), m_Settings.chosenKeys.contains(key));
        m_Monitors.append(entry);
    }
    emit monitorsChanged();
}

void DisplaySettingsModel::setChosen(const QString& key, bool chosen)
{
    if (chosen == m_Settings.chosenKeys.contains(key)) {
        return;
    }
    if (chosen) {
        m_Settings.chosenKeys.append(key);
    }
    else {
        m_Settings.chosenKeys.removeAll(key);
    }
    m_Settings.save();

    // Without monitorsChanged: the check box that called us already shows the change, and the list's delegates
    // must not be recreated from inside its handler
    for (QVariant& entry : m_Monitors) {
        QVariantMap map = entry.toMap();
        if (map.value(QStringLiteral("key")).toString() == key) {
            map.insert(QStringLiteral("chosen"), chosen);
            entry = map;
        }
    }
}

}
