# Tessera multi-display unit tests (docs/phase-3-design.md, "Tests"): layout, refresh rates, display selection,
# stream 0 choice and bitrate totals, plus a smoke test that lists this machine's monitors.
# Built and run on its own (tools/setup/build-qt-client.ps1 -Tests, and ci-client-qt.yml), not by moonlight-qt.pro.
QT += testlib qml quick quickcontrols2
CONFIG += console testcase c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = tessera-tests
DEFINES += SDL_MAIN_HANDLED

APP = $$PWD/../../app
TESSERA = $$APP/streaming/tessera
INCLUDEPATH += $$APP $$TESSERA $$PWD/../../../../common-c/src

SOURCES += \
    tst_tessera.cpp \
    $$TESSERA/displayplan.cpp \
    $$TESSERA/displayselection.cpp \
    $$TESSERA/monitors.cpp \
    $$APP/streaming/streamutils.cpp
HEADERS += \
    $$TESSERA/displayplan.h \
    $$TESSERA/displayselection.h \
    $$TESSERA/monitors.h

win32 {
    INCLUDEPATH += $$PWD/../../libs/windows/include $$PWD/../../libs/windows/include/x64 $$PWD/../../libs/windows/include/x64/SDL2
    LIBS += -L$$PWD/../../libs/windows/lib/x64 -lSDL2 -ldxgi -luser32
    SOURCES += $$TESSERA/monitors_win.cpp
}
macx {
    INCLUDEPATH += $$PWD/../../libs/mac/include $$PWD/../../libs/mac/include/SDL2
    LIBS += -L$$PWD/../../libs/mac/lib -lSDL2 -framework AppKit -framework CoreGraphics -framework ApplicationServices
    QMAKE_RPATHDIR += $$PWD/../../libs/mac/lib
    SOURCES += $$TESSERA/monitors_mac.mm
}
unix:!macx {
    CONFIG += link_pkgconfig
    PKGCONFIG += sdl2
}
