#include "displayselection.h"

#include <QSettings>
#include <QVariantMap>

#include <SDL.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <vector>

#define SER_TESSERA_DISPLAYS "tessera/displays"
#define SER_TESSERA_CHOSEN "tessera/chosenDisplays"
#define SER_TESSERA_RESOLUTION "tessera/displayResolution"
#define SER_TESSERA_MONITORS "tessera/monitors"

namespace tessera {

namespace {

// The settings slider's minimum
constexpr int kMinimumBitrateKbps = 500;

QString refreshText(const Refresh& refresh)
{
    if (refresh.denominator == 1) {
        return QStringLiteral("%1 Hz").arg(refresh.numerator);
    }
    return QStringLiteral("%1 Hz").arg(static_cast<double>(refresh.numerator) / refresh.denominator, 0, 'f', 2);
}

QString sizeText(int width, int height)
{
    return QStringLiteral("%1 × %2").arg(width).arg(height);
}

int wholeFps(const Refresh& refresh)
{
    return static_cast<int>(std::lround(static_cast<double>(refresh.numerator) / refresh.denominator));
}

MonitorSettings::Resolution toResolution(int value, MonitorSettings::Resolution fallback)
{
    return value == MonitorSettings::Native || value == MonitorSettings::DesktopSize || value == MonitorSettings::Lines
               ? static_cast<MonitorSettings::Resolution>(value)
               : fallback;
}

}

bool MonitorSettings::operator==(const MonitorSettings& other) const
{
    return resolution == other.resolution && lines == other.lines && fps == other.fps &&
           bitrateKbps == other.bitrateKbps;
}

MonitorSettings DisplaySettings::monitorSettings(const QString& key) const
{
    const auto it = monitors.constFind(key);
    if (it != monitors.constEnd()) {
        return it.value();
    }
    MonitorSettings result;
    result.resolution = defaultResolution;
    return result;
}

DisplaySettings DisplaySettings::load()
{
    QSettings settings;
    DisplaySettings result;

    const int mode = settings.value(SER_TESSERA_DISPLAYS, ThisDisplay).toInt();
    result.mode = mode == AllDisplays || mode == ChosenDisplays ? static_cast<Mode>(mode) : ThisDisplay;
    result.chosenKeys = settings.value(SER_TESSERA_CHOSEN).toStringList();
    result.defaultResolution = settings.value(SER_TESSERA_RESOLUTION, MonitorSettings::Native).toInt() ==
                                       MonitorSettings::DesktopSize
                                   ? MonitorSettings::DesktopSize
                                   : MonitorSettings::Native;

    settings.beginGroup(QStringLiteral(SER_TESSERA_MONITORS));
    for (const QString& key : settings.childGroups()) {
        MonitorSettings monitor;
        monitor.resolution = toResolution(settings.value(key + QStringLiteral("/resolution")).toInt(),
                                          MonitorSettings::Native);
        monitor.lines = std::max(0, settings.value(key + QStringLiteral("/lines")).toInt());
        monitor.fps = std::max(0, settings.value(key + QStringLiteral("/fps")).toInt());
        monitor.bitrateKbps = std::max(0, settings.value(key + QStringLiteral("/bitrateKbps")).toInt());
        result.monitors.insert(key, monitor);
    }
    settings.endGroup();
    return result;
}

void DisplaySettings::save() const
{
    QSettings settings;
    settings.setValue(SER_TESSERA_DISPLAYS, static_cast<int>(mode));
    settings.setValue(SER_TESSERA_CHOSEN, chosenKeys);
    settings.setValue(SER_TESSERA_RESOLUTION, static_cast<int>(defaultResolution));

    settings.beginGroup(QStringLiteral(SER_TESSERA_MONITORS));
    for (auto it = monitors.constBegin(); it != monitors.constEnd(); ++it) {
        settings.setValue(it.key() + QStringLiteral("/resolution"), static_cast<int>(it.value().resolution));
        settings.setValue(it.key() + QStringLiteral("/lines"), it.value().lines);
        settings.setValue(it.key() + QStringLiteral("/fps"), it.value().fps);
        settings.setValue(it.key() + QStringLiteral("/bitrateKbps"), it.value().bitrateKbps);
    }
    settings.endGroup();
}

StreamMode resolveMode(const ClientMonitor& monitor, const MonitorSettings& settings)
{
    StreamMode mode;
    mode.width = monitor.nativeWidth;
    mode.height = monitor.nativeHeight;
    mode.refresh = monitor.refresh;

    if (settings.resolution == MonitorSettings::DesktopSize && monitor.desktopWidth > 0 && monitor.desktopHeight > 0) {
        mode.width = monitor.desktopWidth;
        mode.height = monitor.desktopHeight;
    }
    else if (settings.resolution == MonitorSettings::Lines && settings.lines > 0 &&
             settings.lines < monitor.nativeHeight) {
        mode.width = widthForHeight(monitor.nativeWidth, monitor.nativeHeight, settings.lines);
        mode.height = settings.lines;
    }

    if (settings.fps > 0 && monitor.refresh.denominator > 0 &&
            settings.fps < static_cast<double>(monitor.refresh.numerator) / monitor.refresh.denominator - 0.5) {
        mode.refresh = {settings.fps, 1};
    }
    return mode;
}

int resolveBitrateKbps(const StreamMode& mode, const MonitorSettings& settings, const BitrateRules& rules)
{
    if (settings.bitrateKbps > 0) {
        return std::clamp(settings.bitrateKbps, kMinimumBitrateKbps, std::max(rules.maximumKbps, kMinimumBitrateKbps));
    }
    if (!rules.defaultKbps) {
        return 0;
    }
    return std::min(rules.defaultKbps(mode.width, mode.height, wholeFps(mode.refresh)), rules.maximumKbps);
}

DisplayPlan planDisplays(const DisplaySettings& settings, const QVector<ClientMonitor>& monitors,
                         QPoint moonlightScreen, bool hdr, const BitrateRules& rules)
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
    std::vector<MonitorSettings> monitorSettings;
    std::vector<StreamMode> modes;
    for (int index : order) {
        const ClientMonitor& monitor = monitors[index];
        monitorSettings.push_back(settings.monitorSettings(QString::fromLatin1(monitor.key.toHex())));
        modes.push_back(resolveMode(monitor, monitorSettings.back()));

        LayoutInput input;
        input.desktop = monitor.desktop;
        input.pixelWidth = modes.back().width;
        input.pixelHeight = modes.back().height;
        layoutInputs.push_back(input);
    }
    const std::vector<Point> positions = layoutInPixels(layoutInputs, 0);

    for (size_t i = 0; i < order.size(); i++) {
        const ClientMonitor& monitor = monitors[order[i]];
        TESSERA_DISPLAY display = {};
        const int keyLength = std::min(static_cast<int>(monitor.key.size()), TESSERA_MONITOR_KEY_MAX_BYTES);
        memcpy(display.key, monitor.key.constData(), keyLength);
        display.keyLength = keyLength;
        display.width = modes[i].width;
        display.height = modes[i].height;
        display.refreshNumerator = modes[i].refresh.numerator;
        display.refreshDenominator = modes[i].refresh.denominator;
        display.x = positions[i].x;
        display.y = positions[i].y;

        // 96 DPI per desktop unit, so the host picks the scale factor the client uses
        const int dpi = static_cast<int>(std::lround(96.0 * monitor.scale * display.width / monitor.nativeWidth));
        display.dpi = std::clamp(dpi, TESSERA_MIN_DPI, TESSERA_MAX_DPI);
        display.hdr = hdr && monitor.hdr;
        display.bitrateKbps = resolveBitrateKbps(modes[i], monitorSettings[i], rules);

        plan.monitors.append(monitor);
        plan.displays.append(display);
    }
    return plan;
}

void logPlan(const DisplayPlan& plan)
{
    for (int i = 0; i < plan.displays.size(); i++) {
        const TESSERA_DISPLAY& display = plan.displays[i];
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Tessera: stream %d is '%s' at %dx%d %d/%d Hz, layout (%d,%d), %d DPI, %d kbps%s",
                    i, qPrintable(plan.monitors[i].name), display.width, display.height,
                    display.refreshNumerator, display.refreshDenominator, display.x, display.y, display.dpi,
                    display.bitrateKbps, display.hdr ? ", HDR" : "");
    }
}

int planBitrateKbps(const DisplayPlan& plan)
{
    long long total = 0;
    for (const TESSERA_DISPLAY& display : plan.displays) {
        total += display.bitrateKbps;
    }
    return static_cast<int>(std::min<long long>(total, INT_MAX));
}

DisplaySettingsModel::DisplaySettingsModel(const std::function<BitrateRules()>& rules, QObject* parent)
    : QObject(parent),
      m_RulesSource(rules),
      m_Settings(DisplaySettings::load())
{
    if (m_RulesSource) {
        m_Rules = m_RulesSource();
    }
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
        rebuild();
    }
}

QVariantList DisplaySettingsModel::monitors() const
{
    return m_Monitors;
}

bool DisplaySettingsModel::active() const
{
    return m_StreamCount > 1;
}

int DisplaySettingsModel::streamCount() const
{
    return m_StreamCount;
}

int DisplaySettingsModel::totalBitrateKbps() const
{
    return m_TotalBitrateKbps;
}

int DisplaySettingsModel::maximumBitrateKbps() const
{
    return m_Rules.maximumKbps;
}

const DisplaySettings& DisplaySettingsModel::settings() const
{
    return m_Settings;
}

void DisplaySettingsModel::refreshMonitors()
{
    setMonitorList(enumerateMonitors());
}

void DisplaySettingsModel::setMonitorList(const QVector<ClientMonitor>& monitors)
{
    m_ClientMonitors = monitors;
    m_Options.clear();
    for (const ClientMonitor& monitor : m_ClientMonitors) {
        MonitorOptions options;
        options.sizes.append({MonitorSettings::Native, 0});
        const bool desktopDiffers = monitor.desktopWidth > 0 && monitor.desktopHeight > 0 &&
                                    (monitor.desktopWidth != monitor.nativeWidth ||
                                     monitor.desktopHeight != monitor.nativeHeight);
        if (desktopDiffers) {
            options.sizes.append({MonitorSettings::DesktopSize, 0});
        }
        for (const Size& size : smallerSizes(monitor.nativeWidth, monitor.nativeHeight)) {
            if (!desktopDiffers || size.height != monitor.desktopHeight) {
                options.sizes.append({MonitorSettings::Lines, size.height});
            }
        }

        options.frameRates.append(0);
        for (int fps : lowerFrameRates(monitor.refresh)) {
            options.frameRates.append(fps);
        }
        m_Options.append(options);
    }
    rebuild();
}

void DisplaySettingsModel::rebuild()
{
    // Upstream's 4:4:4 and unlocked-bitrate settings may have changed since
    if (m_RulesSource) {
        m_Rules = m_RulesSource();
    }

    // Where Moonlight's window is doesn't matter here: it only picks stream 0 when the OS primary isn't streamed
    QPoint moonlightScreen;
    for (const ClientMonitor& monitor : std::as_const(m_ClientMonitors)) {
        if (monitor.primary) {
            moonlightScreen = QPoint(monitor.desktop.x, monitor.desktop.y);
        }
    }
    const DisplayPlan plan = planDisplays(m_Settings, m_ClientMonitors, moonlightScreen, false, m_Rules);
    m_StreamCount = static_cast<int>(plan.displays.size());
    m_TotalBitrateKbps = planBitrateKbps(plan);

    m_Monitors.clear();
    for (int i = 0; i < m_ClientMonitors.size(); i++) {
        const ClientMonitor& monitor = m_ClientMonitors[i];
        const MonitorOptions& options = m_Options[i];
        const QString key = QString::fromLatin1(monitor.key.toHex());
        const MonitorSettings settings = m_Settings.monitorSettings(key);
        const StreamMode mode = resolveMode(monitor, settings);

        bool streamed = false;
        for (const ClientMonitor& planned : plan.monitors) {
            streamed = streamed || planned.key == monitor.key;
        }

        QVariantMap entry;
        entry.insert(QStringLiteral("key"), key);
        entry.insert(QStringLiteral("name"), monitor.name);
        entry.insert(QStringLiteral("primary"), monitor.primary);
        entry.insert(QStringLiteral("hdr"), monitor.hdr);
        entry.insert(QStringLiteral("chosen"), m_Settings.chosenKeys.contains(key));
        entry.insert(QStringLiteral("streamed"), streamed);
        entry.insert(QStringLiteral("x"), monitor.desktop.x);
        entry.insert(QStringLiteral("y"), monitor.desktop.y);
        entry.insert(QStringLiteral("w"), monitor.desktop.w);
        entry.insert(QStringLiteral("h"), monitor.desktop.h);
        entry.insert(QStringLiteral("width"), mode.width);
        entry.insert(QStringLiteral("height"), mode.height);
        entry.insert(QStringLiteral("refresh"), refreshText(mode.refresh));

        // The option showing what the monitor streams at: the one stored, else one of the same height, else native
        QStringList sizeOptions;
        int sizeIndex = -1;
        int sameHeight = -1;
        for (int o = 0; o < options.sizes.size(); o++) {
            const SizeOption& option = options.sizes[o];
            MonitorSettings optionSettings;
            optionSettings.resolution = option.resolution;
            optionSettings.lines = option.lines;
            const StreamMode optionMode = resolveMode(monitor, optionSettings);
            QString text = sizeText(optionMode.width, optionMode.height);
            if (option.resolution == MonitorSettings::Native) {
                text = tr("%1 (native)").arg(text);
            }
            else if (option.resolution == MonitorSettings::DesktopSize) {
                text = tr("%1 (desktop size)").arg(text);
            }
            sizeOptions.append(text);

            if (option.resolution == settings.resolution &&
                    (option.resolution != MonitorSettings::Lines || option.lines == settings.lines)) {
                sizeIndex = o;
            }
            if (sameHeight < 0 && optionMode.height == mode.height) {
                sameHeight = o;
            }
        }
        entry.insert(QStringLiteral("sizeOptions"), sizeOptions);
        entry.insert(QStringLiteral("sizeIndex"), sizeIndex >= 0 ? sizeIndex : std::max(sameHeight, 0));

        QStringList frameRateOptions;
        int frameRateIndex = 0;
        for (int o = 0; o < options.frameRates.size(); o++) {
            const int fps = options.frameRates[o];
            frameRateOptions.append(fps == 0 ? tr("%1 (native)").arg(refreshText(monitor.refresh))
                                             : tr("%1 FPS").arg(fps));
            if (fps != 0 && fps == settings.fps) {
                frameRateIndex = o;
            }
        }
        entry.insert(QStringLiteral("frameRateOptions"), frameRateOptions);
        entry.insert(QStringLiteral("frameRateIndex"), frameRateIndex);

        MonitorSettings automatic = settings;
        automatic.bitrateKbps = 0;
        entry.insert(QStringLiteral("autoBitrate"), settings.bitrateKbps == 0);
        entry.insert(QStringLiteral("bitrateKbps"), resolveBitrateKbps(mode, settings, m_Rules));
        entry.insert(QStringLiteral("defaultBitrateKbps"), resolveBitrateKbps(mode, automatic, m_Rules));
        m_Monitors.append(entry);
    }
    emit monitorsChanged();
}

int DisplaySettingsModel::monitorIndex(const QString& key) const
{
    for (int i = 0; i < m_ClientMonitors.size(); i++) {
        if (QString::fromLatin1(m_ClientMonitors[i].key.toHex()) == key) {
            return i;
        }
    }
    return -1;
}

void DisplaySettingsModel::updateMonitor(const QString& key, const MonitorSettings& settings)
{
    if (m_Settings.monitors.contains(key) && m_Settings.monitors.value(key) == settings) {
        return;
    }
    m_Settings.monitors.insert(key, settings);
    m_Settings.save();
    rebuild();
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
    rebuild();
}

void DisplaySettingsModel::setSize(const QString& key, int index)
{
    const int monitor = monitorIndex(key);
    if (monitor < 0 || index < 0 || index >= m_Options[monitor].sizes.size()) {
        return;
    }
    MonitorSettings settings = m_Settings.monitorSettings(key);
    settings.resolution = m_Options[monitor].sizes[index].resolution;
    settings.lines = m_Options[monitor].sizes[index].lines;
    updateMonitor(key, settings);
}

void DisplaySettingsModel::setFrameRate(const QString& key, int index)
{
    const int monitor = monitorIndex(key);
    if (monitor < 0 || index < 0 || index >= m_Options[monitor].frameRates.size()) {
        return;
    }
    MonitorSettings settings = m_Settings.monitorSettings(key);
    settings.fps = m_Options[monitor].frameRates[index];
    updateMonitor(key, settings);
}

void DisplaySettingsModel::setBitrate(const QString& key, int kbps)
{
    if (monitorIndex(key) < 0) {
        return;
    }
    MonitorSettings settings = m_Settings.monitorSettings(key);
    settings.bitrateKbps = kbps > 0 ? std::clamp(kbps, kMinimumBitrateKbps, std::max(m_Rules.maximumKbps, kMinimumBitrateKbps)) : 0;
    updateMonitor(key, settings);
}

}
