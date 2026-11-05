#include <QtTest>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickStyle>

#include <memory>

#include "displayplan.h"
#include "displayselection.h"
#include "monitors.h"

using namespace tessera;

// Defined in the app's main.cpp; streamutils.cpp refers to it
QAtomicInt g_AsyncLoggingEnabled;

namespace {

LayoutInput input(int x, int y, int w, int h, int pixelWidth, int pixelHeight)
{
    LayoutInput result;
    result.desktop = {x, y, w, h};
    result.pixelWidth = pixelWidth;
    result.pixelHeight = pixelHeight;
    return result;
}

ClientMonitor monitor(const char* id, int x, int y, int w, int h, double scale = 1.0, bool primary = false)
{
    ClientMonitor result;
    result.key = monitorKey(id);
    result.name = QString::fromLatin1(id);
    result.desktop = {x, y, w, h};
    result.scale = scale;
    result.nativeWidth = static_cast<int>(w * scale);
    result.nativeHeight = static_cast<int>(h * scale);
    result.desktopWidth = w;
    result.desktopHeight = h;
    result.refresh = {60, 1};
    result.primary = primary;
    return result;
}

QString hexKey(const char* id)
{
    return QString::fromLatin1(monitorKey(id).toHex());
}

// A stand-in for upstream's default bitrate that is easy to compute by hand
BitrateRules testRules(int maximumKbps = 150000)
{
    BitrateRules rules;
    rules.defaultKbps = [](int width, int height, int fps) {
        return width * height / 1000 * fps / 10;
    };
    rules.maximumKbps = maximumKbps;
    return rules;
}

QVariantMap entryFor(const DisplaySettingsModel& model, const char* id)
{
    for (const QVariant& entry : model.monitors()) {
        if (entry.toMap().value(QStringLiteral("key")).toString() == hexKey(id)) {
            return entry.toMap();
        }
    }
    return QVariantMap();
}

}

class TestTessera : public QObject
{
    Q_OBJECT

    QTemporaryDir m_SettingsDir;

private slots:
    void initTestCase()
    {
        // As the app (main.cpp): the controls' Material style, so the settings section has the app's controls
        QQuickStyle::setStyle(QStringLiteral("Material"));
        qputenv("QT_QUICK_CONTROLS_MATERIAL_THEME", "Dark");

        QCoreApplication::setOrganizationName(QStringLiteral("Tessera tests"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_SettingsDir.path());
    }

    void layoutPixelsPassThrough()
    {
        // Windows: desktop coordinates are pixels
        const std::vector<LayoutInput> monitors = {input(0, 0, 1920, 1080, 1920, 1080),
                                                   input(1920, -200, 2560, 1440, 2560, 1440)};
        auto positions = layoutInPixels(monitors, 0);
        QCOMPARE(positions[0].x, 0);
        QCOMPARE(positions[0].y, 0);
        QCOMPARE(positions[1].x, 1920);
        QCOMPARE(positions[1].y, -200);

        positions = layoutInPixels(monitors, 1);
        QCOMPARE(positions[1].x, 0);
        QCOMPARE(positions[1].y, 0);
        QCOMPARE(positions[0].x, -1920);
        QCOMPARE(positions[0].y, 200);
    }

    void layoutMixedScales()
    {
        // macOS: a Retina laptop (2x) with a 1x monitor to its right, in points
        const std::vector<LayoutInput> monitors = {input(0, 0, 1512, 982, 3024, 1964),
                                                   input(1512, -200, 2560, 1440, 2560, 1440)};
        const auto positions = layoutInPixels(monitors, 0);
        QCOMPARE(positions[1].x, 3024);    // touches the laptop's right edge in pixels
        QCOMPARE(positions[1].y, -400);    // the offset in the laptop's pixels
    }

    void layoutAboveLeftAndChains()
    {
        const std::vector<LayoutInput> monitors = {input(0, 0, 1920, 1080, 1920, 1080),
                                                   input(0, -1080, 1920, 1080, 1920, 1080),
                                                   input(-1280, 0, 1280, 1024, 1280, 1024),
                                                   input(1921, 0, 1920, 1080, 1920, 1080)};  // 1 px gap: still touching
        auto positions = layoutInPixels(monitors, 0);
        QCOMPARE(positions[1].x, 0);
        QCOMPARE(positions[1].y, -1080);
        QCOMPARE(positions[2].x, -1280);
        QCOMPARE(positions[2].y, 0);
        QCOMPARE(positions[3].x, 1920);
        QCOMPARE(positions[3].y, 0);

        // From the rightmost: the left ones are placed through their neighbours
        positions = layoutInPixels(monitors, 3);
        QCOMPARE(positions[0].x, -1920);
        QCOMPARE(positions[2].x, -1920 - 1280);
        QCOMPARE(positions[1].y, -1080);
    }

    void layoutDetachedGoesRight()
    {
        const std::vector<LayoutInput> monitors = {input(0, 0, 1920, 1080, 1920, 1080),
                                                   input(3000, 500, 1280, 720, 1280, 720)};
        const auto positions = layoutInPixels(monitors, 0);
        QCOMPARE(positions[1].x, 1920);
        QCOMPARE(positions[1].y, 0);
    }

    void refreshFractions_data()
    {
        QTest::addColumn<double>("hz");
        QTest::addColumn<int>("numerator");
        QTest::addColumn<int>("denominator");
        QTest::newRow("60") << 60.0 << 60 << 1;
        QTest::newRow("59.94") << 59.94 << 60000 << 1001;
        QTest::newRow("59.951") << 59.951 << 60000 << 1001;
        QTest::newRow("29.97") << 29.97 << 30000 << 1001;
        QTest::newRow("23.976") << 23.976 << 24000 << 1001;
        QTest::newRow("119.88") << 119.88 << 120000 << 1001;
        QTest::newRow("143.981") << 143.981 << 144 << 1;
        QTest::newRow("180") << 180.0 << 180 << 1;
        QTest::newRow("165.08") << 165.08 << 165080 << 1000;
        QTest::newRow("zero") << 0.0 << 60 << 1;
        QTest::newRow("nan") << qQNaN() << 60 << 1;
    }

    void refreshFractions()
    {
        QFETCH(double, hz);
        QFETCH(int, numerator);
        QFETCH(int, denominator);
        const Refresh refresh = refreshFraction(hz);
        QCOMPARE(refresh.numerator, numerator);
        QCOMPARE(refresh.denominator, denominator);
    }

    void streamOrderChoosesStreamZero()
    {
        const std::vector<Rect> desktop = {{-1920, 0, 1920, 1080}, {0, 0, 1920, 1080}, {1920, 0, 1920, 1080}};

        // The OS primary when chosen, then left to right
        QCOMPARE(streamOrder(desktop, {true, true, true}, 1, 2, 4), (std::vector<int>{1, 0, 2}));
        // Else the monitor showing Moonlight
        QCOMPARE(streamOrder(desktop, {true, false, true}, 1, 2, 4), (std::vector<int>{2, 0}));
        // Else the leftmost
        QCOMPARE(streamOrder(desktop, {true, false, true}, 1, 1, 4), (std::vector<int>{0, 2}));
        // At most maxStreams
        QCOMPARE(streamOrder(desktop, {true, true, true}, 1, -1, 2), (std::vector<int>{1, 0}));
        QVERIFY(streamOrder(desktop, {false, false, false}, 1, 1, 4).empty());

        // Equal x: top to bottom
        const std::vector<Rect> stacked = {{0, 1080, 1920, 1080}, {0, 0, 1920, 1080}, {0, -1080, 1920, 1080}};
        QCOMPARE(streamOrder(stacked, {true, true, true}, -1, -1, 4), (std::vector<int>{2, 1, 0}));
    }

    void planThisDisplayStreamsOne()
    {
        DisplaySettings settings;
        const QVector<ClientMonitor> monitors = {monitor("a", 0, 0, 1920, 1080, 1.0, true),
                                                 monitor("b", 1920, 0, 1920, 1080)};
        const DisplayPlan plan = planDisplays(settings, monitors, QPoint(0, 0), false, testRules());
        QVERIFY(plan.displays.isEmpty());
        QVERIFY(plan.warnings.isEmpty());
    }

    void planAllDisplays()
    {
        DisplaySettings settings;
        settings.mode = DisplaySettings::AllDisplays;
        QVector<ClientMonitor> monitors = {monitor("left", -2560, 0, 2560, 1440),
                                           monitor("main", 0, 0, 2560, 1440, 1.5, true)};
        monitors[0].refresh = {180, 1};
        monitors[0].hdr = true;

        DisplayPlan plan = planDisplays(settings, monitors, QPoint(-2560, 0), true, testRules());
        QCOMPARE(plan.displays.size(), 2);
        QCOMPARE(plan.monitors[0].name, QStringLiteral("main"));  // the OS primary is stream 0
        const TESSERA_DISPLAY& main = plan.displays[0];
        QCOMPARE(main.width, 3840);
        QCOMPARE(main.height, 2160);
        QCOMPARE(main.x, 0);
        QCOMPARE(main.dpi, 144);
        QVERIFY(!main.hdr);
        QCOMPARE(main.keyLength, 16);
        QCOMPARE(QByteArray(reinterpret_cast<const char*>(main.key), main.keyLength), monitorKey("main"));
        const TESSERA_DISPLAY& left = plan.displays[1];
        QCOMPARE(left.x, -2560);
        QCOMPARE(left.refreshNumerator, 180);
        QCOMPARE(left.dpi, 96);
        QVERIFY(left.hdr);

        // HDR only when the user turned it on
        plan = planDisplays(settings, monitors, QPoint(0, 0), false, testRules());
        QVERIFY(!plan.displays[1].hdr);
    }

    void planDesktopSize()
    {
        // The single resolution setting of the first test builds still applies to monitors without their own
        DisplaySettings settings;
        settings.mode = DisplaySettings::AllDisplays;
        settings.defaultResolution = MonitorSettings::DesktopSize;
        const QVector<ClientMonitor> monitors = {monitor("a", 0, 0, 2560, 1440, 1.5, true),
                                                 monitor("b", 2560, 0, 1920, 1080)};
        DisplayPlan plan = planDisplays(settings, monitors, QPoint(0, 0), false, testRules());
        QCOMPARE(plan.displays[0].width, 2560);
        QCOMPARE(plan.displays[0].height, 1440);
        QCOMPARE(plan.displays[0].dpi, 96);
        QCOMPARE(plan.displays[1].x, 2560);

        // A monitor's own setting wins
        settings.monitors.insert(hexKey("a"), MonitorSettings());
        plan = planDisplays(settings, monitors, QPoint(0, 0), false, testRules());
        QCOMPARE(plan.displays[0].width, 3840);
        QCOMPARE(plan.displays[1].x, 3840);
    }

    void planPerMonitorModes()
    {
        DisplaySettings settings;
        settings.mode = DisplaySettings::AllDisplays;
        QVector<ClientMonitor> monitors = {monitor("mac", 0, 0, 1512, 982, 2.0, true),
                                           monitor("ext", 1512, 0, 2560, 1440)};
        monitors[1].refresh = {144, 1};

        MonitorSettings mac;
        mac.resolution = MonitorSettings::Lines;
        mac.lines = 1080;
        mac.fps = 30;
        settings.monitors.insert(hexKey("mac"), mac);
        MonitorSettings ext;
        ext.fps = 200;   // above the monitor's refresh rate: ignored
        settings.monitors.insert(hexKey("ext"), ext);

        const DisplayPlan plan = planDisplays(settings, monitors, QPoint(0, 0), false, testRules());
        QCOMPARE(plan.displays[0].width, 1662);
        QCOMPARE(plan.displays[0].height, 1080);
        QCOMPARE(plan.displays[0].refreshNumerator, 30);
        QCOMPARE(plan.displays[0].refreshDenominator, 1);
        QCOMPARE(plan.displays[0].dpi, 106);   // 192 DPI for the native 3024 pixels, scaled to 1662
        QCOMPARE(plan.displays[1].x, 1662);    // the layout uses the streamed sizes
        QCOMPARE(plan.displays[1].refreshNumerator, 144);
    }

    void resolveModes()
    {
        ClientMonitor m = monitor("m", 0, 0, 1512, 982, 2.0);
        MonitorSettings settings;
        StreamMode mode = resolveMode(m, settings);
        QCOMPARE(mode.width, 3024);
        QCOMPARE(mode.height, 1964);

        settings.resolution = MonitorSettings::DesktopSize;
        mode = resolveMode(m, settings);
        QCOMPARE(mode.width, 1512);
        QCOMPARE(mode.height, 982);

        // Lines at or above the native height stream natively
        settings.resolution = MonitorSettings::Lines;
        settings.lines = 2160;
        QCOMPARE(resolveMode(m, settings).height, 1964);
        settings.lines = 720;
        QCOMPARE(resolveMode(m, settings).width, 1108);

        // 59.94 Hz: 30 FPS is lower, 60 isn't
        m.refresh = {60000, 1001};
        settings.fps = 60;
        QCOMPARE(resolveMode(m, settings).refresh.denominator, 1001);
        settings.fps = 30;
        QCOMPARE(resolveMode(m, settings).refresh.numerator, 30);
    }

    void sizeAndFrameRateOptions()
    {
        QCOMPARE(widthForHeight(3024, 1964, 1440), 2218);
        QCOMPARE(widthForHeight(1920, 1080, 720), 1280);
        QCOMPARE(widthForHeight(1920, 0, 720), 0);

        const std::vector<Size> sizes = smallerSizes(3840, 2160);
        QCOMPARE(sizes.size(), size_t(3));
        QCOMPARE(sizes[0].width, 2560);
        QCOMPARE(sizes[0].height, 1440);
        QCOMPARE(sizes[2].height, 720);
        QVERIFY(smallerSizes(1280, 720).empty());

        QCOMPARE(lowerFrameRates({144, 1}), (std::vector<int>{120, 60, 30}));
        QCOMPARE(lowerFrameRates({60000, 1001}), (std::vector<int>{30}));
        QCOMPARE(lowerFrameRates({60, 1}), (std::vector<int>{30}));
        QVERIFY(lowerFrameRates({30, 1}).empty());
    }

    void planChosenDisplays()
    {
        DisplaySettings settings;
        settings.mode = DisplaySettings::ChosenDisplays;
        settings.chosenKeys = {hexKey("b"), hexKey("c"), hexKey("gone")};
        const QVector<ClientMonitor> monitors = {monitor("a", 0, 0, 1920, 1080, 1.0, true),
                                                 monitor("b", 1920, 0, 1920, 1080),
                                                 monitor("c", 3840, 0, 1920, 1080)};

        // The primary isn't chosen: the monitor showing Moonlight is stream 0
        DisplayPlan plan = planDisplays(settings, monitors, QPoint(3840, 0), false, testRules());
        QCOMPARE(plan.monitors.size(), 2);
        QCOMPARE(plan.monitors[0].name, QStringLiteral("c"));
        QCOMPARE(plan.displays[1].x, -1920);
        QVERIFY(plan.warnings.isEmpty());

        // Fewer than two connected: one display, with a warning
        settings.chosenKeys = {hexKey("b")};
        plan = planDisplays(settings, monitors, QPoint(0, 0), false, testRules());
        QVERIFY(plan.displays.isEmpty());
        QCOMPARE(plan.warnings.size(), 1);
    }

    void planLimitsStreams()
    {
        DisplaySettings settings;
        settings.mode = DisplaySettings::AllDisplays;
        QVector<ClientMonitor> monitors;
        for (int i = 0; i < 5; i++) {
            monitors.append(monitor(QByteArray::number(i).constData(), i * 1920, 0, 1920, 1080, 1.0, i == 2));
        }
        const DisplayPlan plan = planDisplays(settings, monitors, QPoint(0, 0), false, testRules());
        QCOMPARE(plan.displays.size(), TESSERA_MAX_STREAMS);
        QCOMPARE(plan.monitors[0].name, QStringLiteral("2"));
        QCOMPARE(plan.warnings.size(), 1);
    }

    void bitrateTotals()
    {
        DisplaySettings settings;
        settings.mode = DisplaySettings::AllDisplays;
        QVector<ClientMonitor> monitors = {monitor("a", 0, 0, 1920, 1080, 1.0, true), monitor("b", 1920, 0, 2560, 1440)};
        monitors[1].refresh = {144, 1};
        DisplayPlan plan = planDisplays(settings, monitors, QPoint(0, 0), false, testRules());

        // Automatic: upstream's default for each display
        QCOMPARE(plan.displays[0].bitrateKbps, 12438);
        QCOMPARE(plan.displays[1].bitrateKbps, 53078);
        QCOMPARE(planBitrateKbps(plan), 12438 + 53078);
        QCOMPARE(planBitrateKbps(DisplayPlan()), 0);

        // The host accepted stream 0 only: the total covers display 0 (MultiDisplay::readLaunchResponse())
        DisplayPlan accepted = plan;
        accepted.displays.resize(1);
        QCOMPARE(planBitrateKbps(accepted), 12438);

        // A default above the slider's maximum is capped at it
        plan = planDisplays(settings, monitors, QPoint(0, 0), false, testRules(50000));
        QCOMPARE(plan.displays[1].bitrateKbps, 50000);

        // A chosen bitrate, kept between the slider's minimum and maximum
        MonitorSettings chosen;
        chosen.bitrateKbps = 20000;
        settings.monitors.insert(hexKey("b"), chosen);
        plan = planDisplays(settings, monitors, QPoint(0, 0), false, testRules());
        QCOMPARE(plan.displays[1].bitrateKbps, 20000);
        QCOMPARE(planBitrateKbps(plan), 12438 + 20000);
        chosen.bitrateKbps = 900000;
        settings.monitors.insert(hexKey("b"), chosen);
        QCOMPARE(planDisplays(settings, monitors, QPoint(0, 0), false, testRules()).displays[1].bitrateKbps, 150000);
        chosen.bitrateKbps = 100;
        settings.monitors.insert(hexKey("b"), chosen);
        QCOMPARE(planDisplays(settings, monitors, QPoint(0, 0), false, testRules()).displays[1].bitrateKbps, 500);

        // Without upstream's rules (and no chosen bitrates) common-c splits the total by pixel rate
        settings.monitors.clear();
        plan = planDisplays(settings, monitors, QPoint(0, 0), false, BitrateRules());
        QCOMPARE(plan.displays[0].bitrateKbps, 0);
    }

    void settingsRoundTrip()
    {
        QCOMPARE(DisplaySettings::load().mode, DisplaySettings::ThisDisplay);
        DisplaySettings settings;
        settings.mode = DisplaySettings::ChosenDisplays;
        settings.chosenKeys = {hexKey("a"), hexKey("b")};
        settings.defaultResolution = MonitorSettings::DesktopSize;
        MonitorSettings a;
        a.resolution = MonitorSettings::Lines;
        a.lines = 1080;
        a.fps = 30;
        a.bitrateKbps = 12000;
        settings.monitors.insert(hexKey("a"), a);
        settings.save();

        const DisplaySettings loaded = DisplaySettings::load();
        QCOMPARE(loaded.mode, DisplaySettings::ChosenDisplays);
        QCOMPARE(loaded.chosenKeys, settings.chosenKeys);
        QCOMPARE(loaded.defaultResolution, MonitorSettings::DesktopSize);
        QCOMPARE(loaded.monitors.size(), 1);
        QVERIFY(loaded.monitorSettings(hexKey("a")) == a);
        // Monitors without their own settings get the default resolution
        QCOMPARE(loaded.monitorSettings(hexKey("b")).resolution, MonitorSettings::DesktopSize);
        QCOMPARE(loaded.monitorSettings(hexKey("b")).bitrateKbps, 0);
    }

    void settingsModel()
    {
        QSettings().clear();
        DisplaySettingsModel model([] { return testRules(); });
        QSignalSpy changed(&model, &DisplaySettingsModel::monitorsChanged);

        QVector<ClientMonitor> monitors = {monitor("mac", 0, 0, 1512, 982, 2.0, true),
                                           monitor("ext", 1512, 0, 2560, 1440)};
        monitors[1].refresh = {144, 1};
        model.setMonitorList(monitors);
        QCOMPARE(changed.count(), 1);
        QVERIFY(!model.active());

        QVariantMap mac = entryFor(model, "mac");
        QCOMPARE(mac.value(QStringLiteral("sizeOptions")).toStringList(),
                 (QStringList{QStringLiteral("3024 × 1964 (native)"), QStringLiteral("1512 × 982 (desktop size)"),
                              QStringLiteral("2218 × 1440"), QStringLiteral("1662 × 1080"),
                              QStringLiteral("1108 × 720")}));
        QCOMPARE(mac.value(QStringLiteral("frameRateOptions")).toStringList(),
                 (QStringList{QStringLiteral("60 Hz (native)"), QStringLiteral("30 FPS")}));
        QCOMPARE(mac.value(QStringLiteral("streamed")).toBool(), false);
        QVariantMap ext = entryFor(model, "ext");
        QCOMPARE(ext.value(QStringLiteral("sizeOptions")).toStringList().size(), 3);
        QCOMPARE(ext.value(QStringLiteral("frameRateOptions")).toStringList().size(), 4);

        model.setMode(DisplaySettings::AllDisplays);
        QVERIFY(model.active());
        QCOMPARE(model.streamCount(), 2);
        QCOMPARE(model.totalBitrateKbps(), 35634 + 53078);
        QVERIFY(entryFor(model, "mac").value(QStringLiteral("streamed")).toBool());

        // Desktop size for the Mac, 60 FPS for the external monitor
        model.setSize(hexKey("mac"), 1);
        mac = entryFor(model, "mac");
        QCOMPARE(mac.value(QStringLiteral("width")).toInt(), 1512);
        QCOMPARE(mac.value(QStringLiteral("sizeIndex")).toInt(), 1);
        model.setFrameRate(hexKey("ext"), 2);
        ext = entryFor(model, "ext");
        QCOMPARE(ext.value(QStringLiteral("refresh")).toString(), QStringLiteral("60 Hz"));
        QCOMPARE(ext.value(QStringLiteral("frameRateIndex")).toInt(), 2);
        QCOMPARE(ext.value(QStringLiteral("bitrateKbps")).toInt(), 22116);
        QCOMPARE(model.totalBitrateKbps(), 8904 + 22116);

        // A chosen bitrate, then back to automatic
        model.setBitrate(hexKey("ext"), 30000);
        ext = entryFor(model, "ext");
        QCOMPARE(ext.value(QStringLiteral("autoBitrate")).toBool(), false);
        QCOMPARE(ext.value(QStringLiteral("bitrateKbps")).toInt(), 30000);
        QCOMPARE(ext.value(QStringLiteral("defaultBitrateKbps")).toInt(), 22116);
        QCOMPARE(model.totalBitrateKbps(), 8904 + 30000);
        model.setBitrate(hexKey("ext"), 900000);
        QCOMPARE(entryFor(model, "ext").value(QStringLiteral("bitrateKbps")).toInt(), 150000);
        model.setBitrate(hexKey("ext"), 0);
        QCOMPARE(entryFor(model, "ext").value(QStringLiteral("autoBitrate")).toBool(), true);

        // Out-of-range indexes and unknown monitors change nothing
        const int before = changed.count();
        model.setSize(hexKey("mac"), 5);
        model.setFrameRate(hexKey("ext"), -1);
        model.setBitrate(hexKey("gone"), 1000);
        QCOMPARE(changed.count(), before);

        // Saved as they change
        const DisplaySettings saved = DisplaySettings::load();
        QCOMPARE(saved.mode, DisplaySettings::AllDisplays);
        QCOMPARE(saved.monitorSettings(hexKey("mac")).resolution, MonitorSettings::DesktopSize);
        QCOMPARE(saved.monitorSettings(hexKey("ext")).fps, 60);
        QCOMPARE(saved.monitorSettings(hexKey("ext")).bitrateKbps, 0);

        // Chosen displays: active once two are chosen
        model.setMode(DisplaySettings::ChosenDisplays);
        QVERIFY(!model.active());
        QCOMPARE(model.totalBitrateKbps(), 0);
        model.setChosen(hexKey("mac"), true);
        QVERIFY(!model.active());
        model.setChosen(hexKey("ext"), true);
        QVERIFY(model.active());
        QVERIFY(entryFor(model, "ext").value(QStringLiteral("chosen")).toBool());

        model.setMode(DisplaySettings::ThisDisplay);
        QVERIFY(!model.active());
        QSettings().clear();
    }

    // The settings section loads without QML errors or warnings
    void settingsSectionLoads()
    {
        qmlRegisterSingletonType<DisplaySettingsModel>("TesseraDisplaySettings", 1, 0, "TesseraDisplaySettings",
                                                       [](QQmlEngine*, QJSEngine*) -> QObject* {
                                                           return new DisplaySettingsModel([] { return testRules(); });
                                                       });
        // AutoResizingComboBox imports these; it uses them only while its popup is open
        for (const char* module : {"SdlGamepadKeyNavigation", "SystemProperties"}) {
            qmlRegisterSingletonType<QObject>(module, 1, 0, module, [](QQmlEngine*, QJSEngine*) -> QObject* {
                return new QObject();
            });
        }

        const QString section = QFINDTESTDATA("../../app/gui/TesseraDisplays.qml");
        QVERIFY(!section.isEmpty());

        QQmlEngine engine;
        QList<QQmlError> warnings;
        connect(&engine, &QQmlEngine::warnings, this, [&warnings](const QList<QQmlError>& list) {
            warnings += list;
        });
        QQmlComponent component(&engine);
        component.setData("import QtQuick 2.9\nColumn { padding: 10; width: 800; TesseraDisplays {} }\n",
                          QUrl::fromLocalFile(QFileInfo(section).absolutePath() + QStringLiteral("/Test.qml")));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));

        auto model = engine.singletonInstance<DisplaySettingsModel*>("TesseraDisplaySettings", "TesseraDisplaySettings");
        QVERIFY(model != nullptr);
        QCOMPARE(model->monitors().size(), enumerateMonitors().size());

        // The per-display controls with two monitors, in each mode
        QVector<ClientMonitor> monitors = {monitor("mac", 0, 0, 1512, 982, 2.0, true),
                                           monitor("ext", 1512, -200, 2560, 1440)};
        model->setMonitorList(monitors);
        for (int mode : {DisplaySettings::AllDisplays, DisplaySettings::ChosenDisplays, DisplaySettings::ThisDisplay}) {
            model->setMode(mode);
            QCoreApplication::processEvents();
        }
        model->setMode(DisplaySettings::AllDisplays);
        model->setBitrate(hexKey("mac"), 20000);
        QCoreApplication::processEvents();
        QSettings().clear();

        // Upstream's files may warn (AutoResizingComboBox reads its style's indicator while it is being created), so
        // only warnings from our files count
        QStringList messages;
        for (const QQmlError& warning : warnings) {
            if (warning.url().fileName() == QStringLiteral("TesseraDisplays.qml") ||
                    warning.url().fileName() == QStringLiteral("Test.qml")) {
                messages += warning.toString();
            }
            else {
                qInfo().noquote() << "upstream QML warning:" << warning.toString();
            }
        }
        QVERIFY2(messages.isEmpty(), qPrintable(messages.join(QLatin1Char('\n'))));
    }

    // Lists this machine's monitors, so a test log shows what the client would report
    void enumerateThisMachine()
    {
        const QVector<ClientMonitor> monitors = enumerateMonitors();
        if (monitors.isEmpty()) {
            QSKIP("no monitors");
        }

        int primaries = 0;
        for (const ClientMonitor& m : monitors) {
            qInfo().noquote() << QStringLiteral("%1 '%2' at (%3,%4) %5x%6: native %7x%8, desktop %9x%10, scale %11, "
                                                "%12/%13 Hz%14%15")
                                     .arg(QString::fromLatin1(m.key.toHex()), m.name)
                                     .arg(m.desktop.x).arg(m.desktop.y).arg(m.desktop.w).arg(m.desktop.h)
                                     .arg(m.nativeWidth).arg(m.nativeHeight).arg(m.desktopWidth).arg(m.desktopHeight)
                                     .arg(m.scale).arg(m.refresh.numerator).arg(m.refresh.denominator)
                                     .arg(m.hdr ? QStringLiteral(", HDR") : QString(),
                                          m.primary ? QStringLiteral(", primary") : QString());
            QCOMPARE(m.key.size(), 16);
            QVERIFY(m.nativeWidth > 0 && m.nativeHeight > 0);
            QVERIFY(m.desktopWidth > 0 && m.desktopWidth <= m.nativeWidth);
            QVERIFY(m.refresh.numerator > 0 && m.refresh.denominator > 0);
            QVERIFY(m.scale >= 1.0);
            primaries += m.primary ? 1 : 0;
        }
        QVERIFY(primaries <= 1);
    }
};

QTEST_MAIN(TestTessera)
#include "tst_tessera.moc"
