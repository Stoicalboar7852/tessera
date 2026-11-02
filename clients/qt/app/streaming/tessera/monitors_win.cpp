#include "monitors.h"

#include <QString>

#include <windows.h>
#include <shellscalingapi.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <vector>

#pragma comment(lib, "shcore.lib")

using Microsoft::WRL::ComPtr;

namespace tessera {

namespace {

struct MonitorSearch {
    RECT bounds;
    HMONITOR found;
};

BOOL CALLBACK matchMonitorBounds(HMONITOR monitor, HDC, LPRECT rect, LPARAM param)
{
    auto search = reinterpret_cast<MonitorSearch*>(param);
    if (EqualRect(rect, &search->bounds)) {
        search->found = monitor;
        return FALSE;
    }
    return TRUE;
}

bool isInternal(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY technology)
{
    return technology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL ||
           technology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED ||
           technology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED;
}

// The monitor's device path, friendly name and refresh rate from the active display configuration
bool describeTarget(const wchar_t* gdiDeviceName, ClientMonitor& monitor)
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG result;
    do {
        UINT32 pathCount = 0, modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) {
            return false;
        }
        paths.resize(pathCount);
        modes.resize(modeCount);
        result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        paths.resize(pathCount);
    } while (result == ERROR_INSUFFICIENT_BUFFER);
    if (result != ERROR_SUCCESS) {
        return false;
    }

    for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
                wcscmp(source.viewGdiDeviceName, gdiDeviceName) != 0) {
            continue;
        }

        DISPLAYCONFIG_TARGET_DEVICE_NAME target = {};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = path.targetInfo.adapterId;
        target.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS) {
            continue;
        }

        // The device path names the monitor (its EDID IDs) and its connection
        const QString devicePath = QString::fromWCharArray(target.monitorDevicePath);
        monitor.key = monitorKey((devicePath.isEmpty() ? QString::fromWCharArray(gdiDeviceName) : devicePath)
                                     .toLower().toUtf8());
        if (target.monitorFriendlyDeviceName[0] != L'\0') {
            monitor.name = QString::fromWCharArray(target.monitorFriendlyDeviceName);
        }
        else if (isInternal(target.outputTechnology)) {
            monitor.name = QStringLiteral("Built-in display");
        }

        const DISPLAYCONFIG_RATIONAL& rate = path.targetInfo.refreshRate;
        if (rate.Numerator != 0 && rate.Denominator != 0) {
            monitor.refresh = refreshFraction(static_cast<double>(rate.Numerator) / rate.Denominator);
        }
        return true;
    }
    return false;
}

// HDR is on when DXGI composes the output in BT.2100 PQ
bool isHdrOn(HMONITOR monitor)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return false;
    }

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; a++) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; o++) {
            ComPtr<IDXGIOutput6> output6;
            DXGI_OUTPUT_DESC1 desc;
            if (SUCCEEDED(output.As(&output6)) && SUCCEEDED(output6->GetDesc1(&desc)) && desc.Monitor == monitor) {
                return desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            }
        }
    }
    return false;
}

}

bool describeMonitor(ClientMonitor& monitor)
{
    // The app is per-monitor DPI aware, so SDL's display bounds are the monitor rectangles in pixels
    MonitorSearch search = {};
    SetRect(&search.bounds, monitor.desktop.x, monitor.desktop.y,
            monitor.desktop.x + monitor.desktop.w, monitor.desktop.y + monitor.desktop.h);
    EnumDisplayMonitors(nullptr, nullptr, matchMonitorBounds, reinterpret_cast<LPARAM>(&search));
    if (search.found == nullptr) {
        return false;
    }

    MONITORINFOEXW info = {};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(search.found, &info)) {
        return false;
    }
    monitor.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;

    if (!describeTarget(info.szDevice, monitor)) {
        monitor.key = monitorKey(QString::fromWCharArray(info.szDevice).toLower().toUtf8());
    }

    UINT dpiX = 0, dpiY = 0;
    if (SUCCEEDED(GetDpiForMonitor(search.found, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) && dpiX > 0) {
        monitor.scale = dpiX / 96.0;
    }

    monitor.hdr = isHdrOn(search.found);
    return true;
}

}
