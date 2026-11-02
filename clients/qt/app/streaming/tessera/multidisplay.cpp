#include "multidisplay.h"

#include "videostats.h"

#include "backend/nvcomputer.h"
#include "backend/nvhttp.h"
#include "streaming/session.h"
#include "streaming/streamutils.h"
#include "streaming/video/decoder.h"

#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <cmath>

// SDL_USEREVENT codes, apart from upstream's (0 and 100-105 in session.cpp)
#define SDL_CODE_TESSERA_STREAM_STATUS 0x5410
#define SDL_CODE_TESSERA_CLEAR_STATUS 0x5411
#define SDL_CODE_TESSERA_FLUSH_BARRIER 0x5412

namespace tessera {

namespace {

constexpr Uint32 kStatusMs = 10000;

// The bitrate slider's maximum in gui/SettingsView.qml
constexpr int kMaximumBitrateKbps = 150000;
constexpr int kUnlockedMaximumBitrateKbps = 500000;

// stopState: 0 while running, else kStopped | reason
constexpr int kStopped = 0x100;

QMutex s_InstanceLock;
MultiDisplay* s_Instance = nullptr;

const char* const k_ReasonTokens[] = TESSERA_REASON_TOKENS;
constexpr int kReasonCount = static_cast<int>(sizeof(k_ReasonTokens) / sizeof(k_ReasonTokens[0]));

const char* reasonToken(int reason)
{
    return reason >= 0 && reason < kReasonCount ? k_ReasonTokens[reason] : "unknown";
}

int reasonFromToken(const QString& token)
{
    for (int i = 0; i < kReasonCount; i++) {
        if (token == QLatin1String(k_ReasonTokens[i])) {
            return i;
        }
    }
    return TESSERA_REASON_UNSUPPORTED;
}

}

MultiDisplay::MultiDisplay(Session* session, const DisplayPlan& plan)
    : m_Session(session),
      m_Plan(plan)
{
    for (int stream = 1; stream < m_Plan.displays.size(); stream++) {
        StreamWindow* streamWindow = new StreamWindow();
        streamWindow->stream = stream;
        streamWindow->monitor = m_Plan.monitors[stream];
        streamWindow->decoderLock = SDL_CreateMutex();
        m_Windows.append(streamWindow);
    }
    m_RequestedStreams = static_cast<int>(m_Plan.displays.size());

    QMutexLocker lock(&s_InstanceLock);
    s_Instance = this;
}

MultiDisplay::~MultiDisplay()
{
    {
        QMutexLocker lock(&s_InstanceLock);
        if (s_Instance == this) {
            s_Instance = nullptr;
        }
    }

    // The windows are normally gone (Session::exec()); this runs after the connection stopped
    destroyWindows();
    for (StreamWindow* streamWindow : std::as_const(m_Windows)) {
        SDL_DestroyMutex(streamWindow->decoderLock);
        delete streamWindow;
    }
}

void MultiDisplay::prepareStreamConfiguration()
{
    const TESSERA_DISPLAY& primary = m_Plan.displays.front();
    STREAM_CONFIGURATION& config = m_Session->m_StreamConfig;

    config.width = primary.width;
    config.height = primary.height;
    // The launch "mode" carries whole frames per second (59.94 Hz as 60)
    config.fps = static_cast<int>(std::lround(static_cast<double>(primary.refreshNumerator) / primary.refreshDenominator));
    config.bitrate = bitrateKbps(static_cast<int>(m_Plan.displays.size()));

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Tessera: streaming %d displays; stream 0 is %dx%dx%d, total bitrate %d kbps",
                static_cast<int>(m_Plan.displays.size()), config.width, config.height, config.fps, config.bitrate);
}

int MultiDisplay::streamZeroDisplayIndex() const
{
    return displayIndexFor(m_Plan.monitors.front());
}

QString MultiDisplay::prepareLaunch(NvComputer* computer)
{
    m_HostSupportsMultiStream = false;
    m_RequestedStreams = 1;

    if (computer->isNvidiaServerSoftware) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Tessera: GeForce Experience hosts stream one display");
        return QString();
    }

    QString serverInfo;
    try {
        NvHTTP http(computer);
        serverInfo = http.getServerInfo(NvHTTP::NVLL_VERBOSE);
    } catch (...) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Tessera: couldn't read the host's serverinfo; streaming one display");
        return QString();
    }

    bool versionOk = false;
    bool maxStreamsOk = false;
    const int version = NvHTTP::getXmlString(serverInfo, TESSERA_SERVERINFO_MULTISTREAM).toInt(&versionOk);
    const int maxStreams = NvHTTP::getXmlString(serverInfo, TESSERA_SERVERINFO_MAX_STREAMS).toInt(&maxStreamsOk);
    if (!versionOk || version < TESSERA_MULTISTREAM_VERSION) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Tessera: the host doesn't advertise multi-stream; streaming one display");
        return QString();
    }
    m_HostSupportsMultiStream = true;
    if (!maxStreamsOk || maxStreams < 2) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Tessera: the host can run %d stream(s) now; streaming one display",
                    maxStreamsOk ? maxStreams : 0);
        m_HostReason = QLatin1String(reasonToken(TESSERA_REASON_HOST_LIMIT));
        return QString();
    }

    // The host takes the first displays it can run and says why it took fewer
    char list[TESSERA_DISPLAY_LIST_MAX_CHARS + 1];
    const int count = static_cast<int>(m_Plan.displays.size());
    if (LiTesseraFormatDisplayList(m_Plan.displays.constData(), count, list, sizeof(list)) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Tessera: the display list is invalid; streaming one display");
        m_HostReason = QLatin1String(reasonToken(TESSERA_REASON_INVALID_REQUEST));
        return QString();
    }

    m_RequestedStreams = count;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Tessera: host allows %d streams; asking for %d: %s",
                maxStreams, count, list);
    return QStringLiteral("&" TESSERA_LAUNCH_ARG_VERSION "=%1&" TESSERA_LAUNCH_ARG_DISPLAYS "=%2")
            .arg(TESSERA_MULTISTREAM_VERSION).arg(QLatin1String(list));
}

void MultiDisplay::readLaunchResponse(const QString& response)
{
    int streams = 1;
    if (m_RequestedStreams > 1) {
        bool ok = false;
        streams = NvHTTP::getXmlString(response, TESSERA_LAUNCH_STREAMS).toInt(&ok);
        if (!ok || streams < 1) {
            streams = 1;
        }
        streams = std::min(streams, m_RequestedStreams);

        const QString reason = NvHTTP::getXmlString(response, TESSERA_LAUNCH_REASON);
        if (!reason.isEmpty()) {
            m_HostReason = reason;
        }
        else if (!ok) {
            m_HostReason = QLatin1String(reasonToken(TESSERA_REASON_UNSUPPORTED));
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Tessera: the host accepted %d of %d streams%s%s",
                    streams, m_RequestedStreams, m_HostReason.isEmpty() ? "" : ": ", qPrintable(m_HostReason));
    }

    m_HostStreams = streams;
    m_RequestedStreams = streams;

    // The automatic bitrate covers the displays that will stream
    const int bitrate = bitrateKbps(streams);
    if (bitrate != m_Session->m_StreamConfig.bitrate) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Tessera: total bitrate %d kbps for %d stream(s)", bitrate, streams);
        m_Session->m_StreamConfig.bitrate = bitrate;
    }
}

bool MultiDisplay::configure(const DECODER_RENDERER_CALLBACKS& stream0Callbacks)
{
    if (m_RequestedStreams < 2) {
        return false;
    }

    // Extra streams are only drawn by decoders that pull their frames (FFmpegVideoDecoder)
    if (!(stream0Callbacks.capabilities & CAPABILITY_PULL_RENDERER)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Tessera: the video decoder can't pull frames; streaming one display");
        m_RequestedStreams = 1;
        return false;
    }

    DECODER_RENDERER_CALLBACKS callbacks[TESSERA_MAX_STREAMS - 1] = {};
    void* contexts[TESSERA_MAX_STREAMS - 1] = {};
    for (int i = 0; i < m_RequestedStreams - 1; i++) {
        callbacks[i] = stream0Callbacks;
        callbacks[i].setup = extraSetup;
        callbacks[i].start = nullptr;
        callbacks[i].stop = nullptr;
        callbacks[i].cleanup = nullptr;
        callbacks[i].submitDecodeUnit = nullptr;
        contexts[i] = m_Windows[i];
    }

    TESSERA_CALLBACKS tesseraCallbacks = {};
    tesseraCallbacks.streamStatus = streamStatus;
    tesseraCallbacks.setHdrMode = setHdrMode;
    if (LiTesseraConfigure(m_Plan.displays.constData(), m_RequestedStreams, callbacks, contexts, &tesseraCallbacks) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Tessera: LiTesseraConfigure() failed; streaming one display");
        m_RequestedStreams = 1;
        return false;
    }
    return true;
}

void MultiDisplay::createWindows(SDL_Surface* icon, bool fullScreen)
{
    {
        QMutexLocker lock(&s_InstanceLock);
        m_EventsEnabled = true;
    }

    const int streams = std::min(LiTesseraGetStreamCount(), m_RequestedStreams);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Tessera: %d of %d displays are streaming",
                streams, static_cast<int>(m_Plan.displays.size()));

    for (StreamWindow* streamWindow : std::as_const(m_Windows)) {
        StreamWindow& w = *streamWindow;
        if (w.stream >= streams) {
            w.stopped = true;
            continue;
        }

        const int stopState = SDL_AtomicGet(&w.stopState);
        if (stopState != 0) {
            closeStream(w, stopState & ~kStopped);
            continue;
        }
        if (SDL_AtomicGet(&w.setUp) == 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Tessera: stream %d was never set up", w.stream);
            closeStream(w, TESSERA_REASON_NO_VIDEO);
            continue;
        }

        int width, height;
        getWindowedSize(w, width, height);
        const int display = displayIndexFor(w.monitor);

#ifdef Q_OS_DARWIN
        const QString title = QObject::tr("%1 (display %2)").arg(m_Session->m_Computer->name).arg(w.stream + 1);
#else
        const QString title = QObject::tr("%1 - Moonlight (display %2)").arg(m_Session->m_Computer->name).arg(w.stream + 1);
#endif
        const Uint32 flags = SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE;
        w.window = SDL_CreateWindow(title.toUtf8().constData(),
                                    SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display),
                                    width, height, flags | StreamUtils::getPlatformWindowFlags());
        if (w.window == nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Tessera: SDL_CreateWindow() failed with platform flags: %s", SDL_GetError());
            w.window = SDL_CreateWindow(title.toUtf8().constData(),
                                        SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display),
                                        width, height, flags);
        }
        if (w.window == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Tessera: SDL_CreateWindow() failed: %s", SDL_GetError());
            closeStream(w, TESSERA_REASON_DECODER_FAILED);
            continue;
        }

        w.windowId = SDL_GetWindowID(w.window);
        w.currentDisplayIndex = SDL_GetWindowDisplayIndex(w.window);

#ifndef Q_OS_DARWIN
        // As for stream 0: before fullscreen, or the icon is lost when leaving it on Windows
        if (icon != nullptr) {
            SDL_SetWindowIcon(w.window, icon);
        }
#else
        Q_UNUSED(icon);
#endif

        // Several fullscreen windows can't each change their monitor's mode, so they cover the desktop instead
        if (fullScreen) {
            SDL_SetWindowFullscreen(w.window, SDL_WINDOW_FULLSCREEN_DESKTOP);
        }

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Tessera: stream %d window %u on display %d ('%s'), %dx%d",
                    w.stream, w.windowId, w.currentDisplayIndex, qPrintable(w.monitor.name), width, height);
    }

    // Keyboard focus starts on stream 0's window, as in a single-display session
    SDL_RaiseWindow(m_Session->m_Window);

    if (!m_HostSupportsMultiStream) {
        showStatus(QObject::tr("%1 can't stream several displays (it isn't a Tessera host). Streaming %2 only.")
                       .arg(m_Session->m_Computer->name, m_Plan.monitors.front().name));
    }
    else if (streams < m_Plan.displays.size()) {
        showStatus(QObject::tr("Streaming %1 of %2 displays: %3.")
                       .arg(streams).arg(m_Plan.displays.size()).arg(reasonText(reasonFromToken(m_HostReason))));
    }
}

bool MultiDisplay::handleEvent(SDL_Event& event)
{
    // A status shown before stream 0's renderer existed is drawn once it does
    if (m_PendingStatus && m_Session->m_VideoDecoder != nullptr) {
        m_PendingStatus = false;
        m_Session->getOverlayManager().setOverlayTextUpdated(Overlay::OverlayStatusUpdate);
    }

    switch (event.type) {
    case SDL_USEREVENT:
        switch (event.user.code) {
        case SDL_CODE_FRAME_READY:
            // Pacers of main-thread renderers post this without saying which; a renderer with no new frame does nothing
            for (StreamWindow* streamWindow : std::as_const(m_Windows)) {
                if (streamWindow->decoder != nullptr) {
                    streamWindow->decoder->renderFrameOnMainThread();
                }
            }
            return false;

        case SDL_CODE_TESSERA_FLUSH_BARRIER: {
            StreamWindow* streamWindow = windowForStream(static_cast<int>(reinterpret_cast<intptr_t>(event.user.data1)));
            if (streamWindow != nullptr && streamWindow->flushingWindowEvents > 0) {
                streamWindow->flushingWindowEvents--;
            }
            return true;
        }

        case SDL_CODE_TESSERA_STREAM_STATUS: {
            const int stream = static_cast<int>(reinterpret_cast<intptr_t>(event.user.data1));
            const int status = static_cast<int>(reinterpret_cast<intptr_t>(event.user.data2));
            StreamWindow* streamWindow = windowForStream(stream);
            if (streamWindow != nullptr && (status >> 16) == TESSERA_STREAM_STOPPED) {
                closeStream(*streamWindow, status & 0xFFFF);
            }
            return true;
        }

        case SDL_CODE_TESSERA_CLEAR_STATUS:
            if (static_cast<int>(reinterpret_cast<intptr_t>(event.user.data1)) == m_StatusGeneration) {
                m_StatusTimer = 0;
                m_PendingStatus = false;
                if (m_Session->m_MouseEmulationRefCount == 0) {
                    m_Session->getOverlayManager().setOverlayState(Overlay::OverlayStatusUpdate, false);
                }
            }
            return true;
        }
        return false;

    case SDL_WINDOWEVENT: {
        const Uint32 windowId = event.window.windowID;
        if (m_ClosedWindowIds.contains(windowId)) {
            return true;
        }

        // Closing any of the session's windows ends the session (SDL only quits when the last one closes)
        if (event.window.event == SDL_WINDOWEVENT_CLOSE && ownsWindow(windowId)) {
            requestQuit();
            return true;
        }

        StreamWindow* streamWindow = findWindow(windowId);
        if (streamWindow == nullptr) {
            // Focus moving from stream 0's window to another of ours is not a focus loss
            return event.window.event == SDL_WINDOWEVENT_FOCUS_LOST && ownsWindow(windowId) && keyboardFocusIsOurs();
        }

        switch (event.window.event) {
        case SDL_WINDOWEVENT_FOCUS_LOST:
            if (keyboardFocusIsOurs()) {
                return true;
            }
            // Fall through
        case SDL_WINDOWEVENT_FOCUS_GAINED:
        case SDL_WINDOWEVENT_ENTER:
        case SDL_WINDOWEVENT_LEAVE:
            // Session-wide input state: upstream handles these as if they were stream 0's
            event.window.windowID = SDL_GetWindowID(m_Session->m_Window);
            return false;
        default:
            handleWindowEvent(*streamWindow, event.window);
            return true;
        }
    }

    case SDL_RENDER_DEVICE_RESET:
        // Device loss affects every window; upstream then recreates stream 0's decoder
        for (StreamWindow* streamWindow : std::as_const(m_Windows)) {
            if (streamWindow->window != nullptr && !streamWindow->stopped) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Tessera: recreating stream %d's renderer after a device reset",
                            streamWindow->stream);
                if (!recreateDecoder(*streamWindow)) {
                    closeStream(*streamWindow, TESSERA_REASON_DECODER_FAILED);
                }
            }
        }
        return false;

    case SDL_DISPLAYEVENT:
        if (event.display.event == SDL_DISPLAYEVENT_DISCONNECTED) {
            checkMonitorsPresent();
        }
        return false;
    }

    return false;
}

void MultiDisplay::setFullScreen(bool fullScreen)
{
    for (StreamWindow* streamWindow : std::as_const(m_Windows)) {
        StreamWindow& w = *streamWindow;
        if (w.window == nullptr) {
            continue;
        }

#if defined(Q_OS_WIN32) || defined(Q_OS_DARWIN)
        // As Session::toggleFullscreen() does for stream 0; the size change recreates it
        SDL_LockMutex(w.decoderLock);
        delete w.decoder;
        w.decoder = nullptr;
        SDL_UnlockMutex(w.decoderLock);
#endif

        SDL_SetWindowFullscreen(w.window, fullScreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        if (!fullScreen) {
            int width, height;
            getWindowedSize(w, width, height);
            const int display = displayIndexFor(w.monitor);
            SDL_SetWindowSize(w.window, width, height);
            SDL_SetWindowPosition(w.window, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));
        }
    }
}

void MultiDisplay::destroyWindows()
{
    {
        QMutexLocker lock(&s_InstanceLock);
        m_EventsEnabled = false;
    }

    if (m_StatusTimer != 0) {
        SDL_RemoveTimer(m_StatusTimer);
        m_StatusTimer = 0;
    }

    for (StreamWindow* streamWindow : std::as_const(m_Windows)) {
        SDL_LockMutex(streamWindow->decoderLock);
        delete streamWindow->decoder;
        streamWindow->decoder = nullptr;
        SDL_UnlockMutex(streamWindow->decoderLock);

        if (streamWindow->window != nullptr) {
            SDL_DestroyWindow(streamWindow->window);
            streamWindow->window = nullptr;
        }
    }
}

bool MultiDisplay::ownsWindow(Uint32 windowId) const
{
    return streamForWindow(windowId) >= 0;
}

int MultiDisplay::streamForWindow(Uint32 windowId) const
{
    if (m_Session->m_Window != nullptr && windowId == SDL_GetWindowID(m_Session->m_Window)) {
        return 0;
    }
    const StreamWindow* streamWindow = findWindow(windowId);
    return streamWindow != nullptr ? streamWindow->stream : -1;
}

bool MultiDisplay::getStreamSize(int stream, int& width, int& height) const
{
    if (stream == 0) {
        width = m_Session->m_ActiveVideoWidth;
        height = m_Session->m_ActiveVideoHeight;
        return width > 0 && height > 0;
    }

    const StreamWindow* streamWindow = windowForStream(stream);
    if (streamWindow == nullptr || SDL_AtomicGet(const_cast<SDL_atomic_t*>(&streamWindow->setUp)) == 0) {
        return false;
    }
    width = streamWindow->width;
    height = streamWindow->height;
    return width > 0 && height > 0;
}

MultiDisplay::StreamWindow* MultiDisplay::findWindow(Uint32 windowId) const
{
    if (windowId == 0) {
        return nullptr;
    }
    for (StreamWindow* streamWindow : m_Windows) {
        if (streamWindow->window != nullptr && streamWindow->windowId == windowId) {
            return streamWindow;
        }
    }
    return nullptr;
}

MultiDisplay::StreamWindow* MultiDisplay::windowForStream(int stream) const
{
    return stream >= 1 && stream <= m_Windows.size() ? m_Windows[stream - 1] : nullptr;
}

int MultiDisplay::displayIndexFor(const ClientMonitor& monitor) const
{
    // Display indexes can change between enumeration and now; the bounds identify the monitor
    const int displays = SDL_GetNumVideoDisplays();
    for (int i = 0; i < displays; i++) {
        SDL_Rect bounds;
        if (SDL_GetDisplayBounds(i, &bounds) == 0 &&
                bounds.x == monitor.desktop.x && bounds.y == monitor.desktop.y &&
                bounds.w == monitor.desktop.w && bounds.h == monitor.desktop.h) {
            return i;
        }
    }
    return monitor.sdlIndex >= 0 && monitor.sdlIndex < displays ? monitor.sdlIndex : 0;
}

void MultiDisplay::getWindowedSize(const StreamWindow& streamWindow, int& width, int& height) const
{
    const TESSERA_DISPLAY& display = m_Plan.displays[streamWindow.stream];
    width = streamWindow.width > 0 ? streamWindow.width : display.width;
    height = streamWindow.height > 0 ? streamWindow.height : display.height;

    const int displayIndex = displayIndexFor(streamWindow.monitor);
    SDL_Rect usableBounds, bounds;
    if (SDL_GetDisplayUsableBounds(displayIndex, &usableBounds) != 0 || SDL_GetDisplayBounds(displayIndex, &bounds) != 0) {
        return;
    }

    // As Session::getWindowDimensions(): the stream's size if it fits, else 80 % of the usable area. A window the size
    // of the whole monitor wouldn't change size when leaving fullscreen (and its decoder wouldn't be recreated).
    if (width <= usableBounds.w && height <= usableBounds.h && (width < bounds.w || height < bounds.h)) {
        return;
    }

    SDL_Rect src, dst;
    src.x = src.y = dst.x = dst.y = 0;
    src.w = width;
    src.h = height;
    dst.w = ((int)(usableBounds.w * 0.80f)) & ~0x1;
    dst.h = ((int)(usableBounds.h * 0.80f)) & ~0x1;
    StreamUtils::scaleSourceToDestinationSurface(&src, &dst);
    width = dst.w;
    height = dst.h;
}

int MultiDisplay::bitrateKbps(int streams) const
{
    const StreamingPreferences* preferences = m_Session->m_Preferences;
    DisplayPlan plan = m_Plan;
    plan.displays.resize(std::min(streams, static_cast<int>(plan.displays.size())));
    return planBitrateKbps(plan, preferences->autoAdjustBitrate, preferences->bitrateKbps,
                           preferences->unlockBitrate ? kUnlockedMaximumBitrateKbps : kMaximumBitrateKbps,
                           [preferences](int width, int height, int fps) {
                               return StreamingPreferences::getDefaultBitrate(width, height, fps,
                                                                              preferences->enableYUV444);
                           });
}

void MultiDisplay::flushWindowEvents(StreamWindow& streamWindow)
{
    // As Session::flushWindowEvents(), for one window
    SDL_PumpEvents();
    streamWindow.flushingWindowEvents++;

    SDL_Event flushEvent = {};
    flushEvent.type = SDL_USEREVENT;
    flushEvent.user.code = SDL_CODE_TESSERA_FLUSH_BARRIER;
    flushEvent.user.data1 = reinterpret_cast<void*>(static_cast<intptr_t>(streamWindow.stream));
    SDL_PushEvent(&flushEvent);
}

bool MultiDisplay::recreateDecoder(StreamWindow& w)
{
    const StreamingPreferences* preferences = m_Session->m_Preferences;

    SDL_LockMutex(w.decoderLock);

    delete w.decoder;
    w.decoder = nullptr;

    // Discard the window events the old renderer's teardown queued, as upstream does for stream 0
    flushWindowEvents(w);
    w.currentDisplayIndex = SDL_GetWindowDisplayIndex(w.window);

    // If the stream exceeds the display refresh rate (plus some slack), V-sync would hold it back
    const int displayHz = StreamUtils::getDisplayRefreshRate(w.window);
    bool enableVsync = preferences->enableVsync;
    if (displayHz + 5 < w.frameRate) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Tessera: disabling V-sync for stream %d (%d Hz display, %d FPS stream)",
                    w.stream, displayHz, w.frameRate);
        enableVsync = false;
    }

    if (!Session::chooseDecoder(preferences->videoDecoderSelection, preferences->rendererSelection,
                                w.window, w.videoFormat, w.width, w.height, w.frameRate,
                                enableVsync, enableVsync && preferences->framePacing, false,
                                w.decoder, w.stream)) {
        SDL_UnlockMutex(w.decoderLock);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Tessera: no video decoder for stream %d", w.stream);
        return false;
    }

    // Complete the reset with an IDR frame, and catch an HDR change made while there was no decoder
    LiTesseraRequestIdrFrame(w.stream);
    w.decoder->setHdrMode(LiTesseraGetCurrentHostDisplayHdrMode(w.stream));

    SDL_UnlockMutex(w.decoderLock);
    return true;
}

void MultiDisplay::handleWindowEvent(StreamWindow& w, const SDL_WindowEvent& event)
{
    // As upstream's handling of stream 0's window in Session::exec(): recreate the decoder for size and display changes
    // and the first SHOWN event, unless the renderer can adapt
    if (event.event != SDL_WINDOWEVENT_SIZE_CHANGED &&
            (event.event != SDL_WINDOWEVENT_SHOWN || w.decoder != nullptr)) {
#if SDL_VERSION_ATLEAST(2, 0, 18)
        if (event.event != SDL_WINDOWEVENT_DISPLAY_CHANGED) {
            return;
        }
#else
        if (SDL_GetWindowDisplayIndex(w.window) == w.currentDisplayIndex) {
            return;
        }
#endif
    }
#ifdef Q_OS_WIN32
    else if (SDL_GetWindowFlags(w.window) & SDL_WINDOW_MINIMIZED) {
        return;
    }
#endif

    // A window without a decoder always gets one, even during a flush
    if (w.flushingWindowEvents > 0 && w.decoder != nullptr) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Tessera: stream %d dropping window event during flush: %d (%d %d)",
                    w.stream, event.event, event.data1, event.data2);
        return;
    }

    if (w.decoder != nullptr) {
        bool forceRecreation = false;

        WINDOW_STATE_CHANGE_INFO windowChangeInfo = {};
        windowChangeInfo.window = w.window;
        if (event.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            windowChangeInfo.stateChangeFlags |= WINDOW_STATE_CHANGE_SIZE;
            windowChangeInfo.width = event.data1;
            windowChangeInfo.height = event.data2;
        }

        const int newDisplayIndex = SDL_GetWindowDisplayIndex(w.window);
        if (newDisplayIndex != w.currentDisplayIndex) {
            windowChangeInfo.stateChangeFlags |= WINDOW_STATE_CHANGE_DISPLAY;
            windowChangeInfo.displayIndex = newDisplayIndex;

            // A different refresh rate needs a new pacer
            SDL_DisplayMode oldMode, newMode;
            if (SDL_GetCurrentDisplayMode(w.currentDisplayIndex, &oldMode) < 0 ||
                    SDL_GetCurrentDisplayMode(newDisplayIndex, &newMode) < 0 ||
                    oldMode.refresh_rate != newMode.refresh_rate) {
                forceRecreation = true;
            }
        }

        if (!forceRecreation && w.decoder->notifyWindowChanged(&windowChangeInfo)) {
            w.currentDisplayIndex = newDisplayIndex;
            return;
        }
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Tessera: recreating stream %d's renderer for window event: %d (%d %d)",
                w.stream, event.event, event.data1, event.data2);
    if (!recreateDecoder(w)) {
        closeStream(w, TESSERA_REASON_DECODER_FAILED);
    }
}

void MultiDisplay::closeStream(StreamWindow& w, int reason)
{
    if (w.stopped) {
        return;
    }
    w.stopped = true;

    SDL_LockMutex(w.decoderLock);
    delete w.decoder;
    w.decoder = nullptr;
    SDL_UnlockMutex(w.decoderLock);

    if (w.window != nullptr) {
        const bool hadFocus = SDL_GetKeyboardFocus() == w.window;
        m_ClosedWindowIds.append(w.windowId);
        SDL_DestroyWindow(w.window);
        w.window = nullptr;
        if (hadFocus) {
            SDL_RaiseWindow(m_Session->m_Window);
        }
    }

    clearVideoStats(w.stream);
    showStatus(QObject::tr("Display %1 stopped: %2.").arg(w.stream + 1).arg(reasonText(reason)));
}

bool MultiDisplay::keyboardFocusIsOurs() const
{
    // The OS may report the old window's focus loss before the new window's gain
    SDL_PumpEvents();
    SDL_Window* focus = SDL_GetKeyboardFocus();
    return focus != nullptr && ownsWindow(SDL_GetWindowID(focus));
}

void MultiDisplay::checkMonitorsPresent()
{
    // v1 can't change a session's streams, so losing a streamed monitor ends the session
    const QVector<ClientMonitor> monitors = enumerateMonitors();
    for (int stream = 0; stream < m_Plan.monitors.size(); stream++) {
        const StreamWindow* streamWindow = windowForStream(stream);
        if (stream > 0 && (streamWindow == nullptr || streamWindow->stopped)) {
            continue;
        }

        const ClientMonitor& streamed = m_Plan.monitors[stream];
        const bool present = std::any_of(monitors.cbegin(), monitors.cend(), [&streamed](const ClientMonitor& monitor) {
            return monitor.key == streamed.key;
        });
        if (!present) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Tessera: streamed display '%s' (stream %d) was disconnected",
                        qPrintable(streamed.name), stream);
            emit m_Session->displayLaunchError(
                    QObject::tr("The display %1 was disconnected, so the session ended. Start it again to stream the "
                                "displays that are connected now.").arg(streamed.name));
            requestQuit();
            return;
        }
    }
}

void MultiDisplay::showStatus(const QString& text)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Tessera: %s", qPrintable(text));

    // The gamepad mouse mode message uses the same overlay and stays until that mode ends
    if (m_Session->m_MouseEmulationRefCount > 0) {
        return;
    }

    Overlay::OverlayManager& overlay = m_Session->getOverlayManager();
    overlay.updateOverlayText(Overlay::OverlayStatusUpdate, text.toUtf8().constData());
    overlay.setOverlayState(Overlay::OverlayStatusUpdate, true);
    m_PendingStatus = m_Session->m_VideoDecoder == nullptr;

    if (m_StatusTimer != 0) {
        SDL_RemoveTimer(m_StatusTimer);
    }
    m_StatusGeneration++;
    m_StatusTimer = SDL_AddTimer(kStatusMs, clearStatusTimer, reinterpret_cast<void*>(static_cast<intptr_t>(m_StatusGeneration)));
}

QString MultiDisplay::reasonText(int reason)
{
    switch (reason) {
    case TESSERA_REASON_HOST_LIMIT:
        return QObject::tr("the host streams fewer displays at once");
    case TESSERA_REASON_ENCODER_LIMIT:
        return QObject::tr("the host's GPU can't encode more streams");
    case TESSERA_REASON_NOT_PERMITTED:
        return QObject::tr("multi-display is not enabled for this client in the host's web UI");
    case TESSERA_REASON_NO_DISPLAY_DRIVER:
        return QObject::tr("the host's virtual display driver is not installed");
    case TESSERA_REASON_DISPLAY_FAILED:
        return QObject::tr("the host couldn't create the display");
    case TESSERA_REASON_ENCODER_FAILED:
        return QObject::tr("the host's video encoder failed");
    case TESSERA_REASON_CAPTURE_FAILED:
        return QObject::tr("the host couldn't capture the display");
    case TESSERA_REASON_INVALID_REQUEST:
        return QObject::tr("the host rejected the list of displays");
    case TESSERA_REASON_DECODER_FAILED:
        return QObject::tr("this computer couldn't start a video decoder for it");
    case TESSERA_REASON_NO_VIDEO:
        return QObject::tr("no video arrived");
    case TESSERA_REASON_UNSUPPORTED:
    default:
        return QObject::tr("the host doesn't support it");
    }
}

void MultiDisplay::requestQuit()
{
    // SDL queues its own SDL_QUIT when the last window closes; a second one would end the next session
    if (!SDL_HasEvent(SDL_QUIT)) {
        SDL_Event quitEvent = {};
        quitEvent.type = SDL_QUIT;
        SDL_PushEvent(&quitEvent);
    }
}

int MultiDisplay::extraSetup(int videoFormat, int width, int height, int frameRate, void* context, int)
{
    StreamWindow* streamWindow = static_cast<StreamWindow*>(context);
    if (streamWindow == nullptr) {
        return -1;
    }

    // As Session::drSetup(): the decoder is created on the main thread once the window is shown
    streamWindow->videoFormat = videoFormat;
    streamWindow->width = width;
    streamWindow->height = height;
    streamWindow->frameRate = frameRate;
    SDL_AtomicSet(&streamWindow->setUp, 1);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Tessera: stream %d video is %dx%dx%d (format 0x%x)",
                streamWindow->stream, width, height, frameRate, videoFormat);
    return 0;
}

void MultiDisplay::streamStatus(int stream, int state, int reason)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Tessera: stream %d %s (%s)", stream,
                state == TESSERA_STREAM_STOPPED ? "stopped" : "running", reasonToken(reason));

    QMutexLocker lock(&s_InstanceLock);
    if (s_Instance == nullptr) {
        return;
    }
    StreamWindow* streamWindow = s_Instance->windowForStream(stream);
    if (streamWindow == nullptr) {
        return;
    }

    // Before the windows exist, createWindows() reads the state; after, the main thread hears of it
    SDL_AtomicSet(&streamWindow->stopState, state == TESSERA_STREAM_STOPPED ? kStopped | (reason & 0xFF) : 0);
    if (s_Instance->m_EventsEnabled) {
        SDL_Event event = {};
        event.type = SDL_USEREVENT;
        event.user.code = SDL_CODE_TESSERA_STREAM_STATUS;
        event.user.data1 = reinterpret_cast<void*>(static_cast<intptr_t>(stream));
        event.user.data2 = reinterpret_cast<void*>(static_cast<intptr_t>((state << 16) | (reason & 0xFFFF)));
        SDL_PushEvent(&event);
    }
}

void MultiDisplay::setHdrMode(int stream, bool enabled)
{
    QMutexLocker lock(&s_InstanceLock);
    if (s_Instance == nullptr) {
        return;
    }
    StreamWindow* streamWindow = s_Instance->windowForStream(stream);
    if (streamWindow == nullptr) {
        return;
    }

    // As Session::clSetHdrMode(): a decoder being recreated picks the mode up when it's done
    if (SDL_TryLockMutex(streamWindow->decoderLock) == 0) {
        if (streamWindow->decoder != nullptr) {
            streamWindow->decoder->setHdrMode(enabled);
        }
        SDL_UnlockMutex(streamWindow->decoderLock);
    }
}

Uint32 MultiDisplay::clearStatusTimer(Uint32, void* param)
{
    SDL_Event event = {};
    event.type = SDL_USEREVENT;
    event.user.code = SDL_CODE_TESSERA_CLEAR_STATUS;
    event.user.data1 = param;
    SDL_PushEvent(&event);
    return 0;
}

}
