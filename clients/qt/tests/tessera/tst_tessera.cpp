#include <QtTest>
#include <QQmlComponent>
#include <QQmlEngine>

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

}

class TestTessera : public QObject
{
    Q_OBJECT

    QTemporaryDir m_SettingsDir;

private slots:
    void initTestCase()
    {
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
        const DisplayPlan plan = planDisplays(settings, monitors, QPoint(0, 0), false);
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

        DisplayPlan plan = planDisplays(settings, monitors, QPoint(-2560, 0), true);
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
        plan = planDisplays(settings, monitors, QPoint(0, 0), false);
        QVERIFY(!plan.displays[1].hdr);
    }

    void planDesktopSize()
    {
        DisplaySettings settings;
        settings.mode = DisplaySettings::AllDisplays;
        settings.resolution = DisplaySettings::DesktopSize;
        const QVector<ClientMonitor> monitors = {monitor("a", 0, 0, 2560, 1440, 1.5, true),
                                                 monitor("b", 2560, 0, 1920, 1080)};
        const DisplayPlan plan = planDisplays(settings, monitors, QPoint(0, 0), false);
        QCOMPARE(plan.displays[0].width, 2560);
        QCOMPARE(plan.displays[0].height, 1440);
        QCOMPARE(plan.displays[0].dpi, 96);
        QCOMPARE(plan.displays[1].x, 2560);
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
        DisplayPlan plan = planDisplays(settings, monitors, QPoint(3840, 0), false);
        QCOMPARE(plan.monitors.size(), 2);
        QCOMPARE(plan.monitors[0].name, QStringLiteral("c"));
        QCOMPARE(plan.displays[1].x, -1920);
        QVERIFY(plan.warnings.isEmpty());

        // Fewer than two connected: one display, with a warning
        settings.chosenKeys = {hexKey("b")};
        plan = planDisplays(settings, monitors, QPoint(0, 0), false);
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
        const DisplayPlan plan = planDisplays(settings, monitors, QPoint(0, 0), false);
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
        const DisplayPlan plan = planDisplays(settings, monitors, QPoint(0, 0), false);
        const auto defaultKbps = [](int width, int height, int fps) {
            return width * height / 1000 * fps / 10;
        };

        QCOMPARE(planBitrateKbps(plan, true, 20000, 150000, defaultKbps), 12438 + 53078);
        QCOMPARE(planBitrateKbps(plan, true, 20000, 50000, defaultKbps), 50000);
        QCOMPARE(planBitrateKbps(plan, false, 20000, 150000, defaultKbps), 20000);
        QCOMPARE(planBitrateKbps(DisplayPlan(), true, 20000, 150000, defaultKbps), 20000);
    }

    void settingsRoundTrip()
    {
        QCOMPARE(DisplaySettings::load().mode, DisplaySettings::ThisDisplay);
        DisplaySettings settings;
        settings.mode = DisplaySettings::ChosenDisplays;
        settings.chosenKeys = {hexKey("a"), hexKey("b")};
        settings.resolution = DisplaySettings::DesktopSize;
        settings.save();

        const DisplaySettings loaded = DisplaySettings::load();
        QCOMPARE(loaded.mode, DisplaySettings::ChosenDisplays);
        QCOMPARE(loaded.chosenKeys, settings.chosenKeys);
        QCOMPARE(loaded.resolution, DisplaySettings::DesktopSize);
    }

    // The settings section loads without QML errors or warnings
    void settingsSectionLoads()
    {
        qmlRegisterSingletonType<DisplaySettingsModel>("TesseraDisplaySettings", 1, 0, "TesseraDisplaySettings",
                                                       [](QQmlEngine*, QJSEngine*) -> QObject* {
                                                           return new DisplaySettingsModel();
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

        // Upstream's AutoResizingComboBox warns once on creation (it reads parent.width before it has a parent), so
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

        auto model = engine.singletonInstance<DisplaySettingsModel*>("TesseraDisplaySettings", "TesseraDisplaySettings");
        QVERIFY(model != nullptr);
        QCOMPARE(model->monitors().size(), enumerateMonitors().size());
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
