// Tessera multi-display: a session's extra streams, each with its own window and decoder on its own monitor
// (docs/phase-3-design.md, component 5). Stream 0 stays upstream's window and decoder.
#pragma once

#include "displayselection.h"

#include "SDL_compat.h"

#include <QString>
#include <QVector>

#include <Limelight.h>
#include <Tessera.h>

class Session;
class NvComputer;
class IVideoDecoder;

namespace tessera {

class MultiDisplay
{
public:
    // plan.displays has at least two entries
    MultiDisplay(Session* session, const DisplayPlan& plan);
    ~MultiDisplay();

    // Main thread, in Session::initialize(): stream 0 runs at display 0's mode, and the bitrate covers every display
    void prepareStreamConfiguration();

    // The SDL display for stream 0's window
    int streamZeroDisplayIndex() const;

    // Connection thread, before the launch request: the launch arguments that ask a Tessera host for one stream per
    // display, or an empty string if the host can't stream several.
    QString prepareLaunch(NvComputer* computer);

    // Connection thread: the host's reply to the launch request. Lowers the automatic bitrate if the host accepted
    // fewer streams.
    void readLaunchResponse(const QString& response);

    // Connection thread, before LiStartConnection(): sets up common-c for the extra streams. Returns false if the session
    // streams one display; the caller then calls LiTesseraReset().
    bool configure(const DECODER_RENDERER_CALLBACKS& stream0Callbacks);

    // Main thread, once the connection is up and stream 0's window exists: one window per extra stream
    void createWindows(SDL_Surface* icon, bool fullScreen);

    // Main thread: handles the events for the extra streams. Returns true if the event was consumed; the caller then
    // skips it. Window events for the extra windows that upstream should see (focus, mouse entering and leaving) are
    // rewritten to stream 0's window.
    bool handleEvent(SDL_Event& event);

    // Main thread: the fullscreen toggle, after stream 0's window changed
    void setFullScreen(bool fullScreen);

    // Main thread: destroys the extra windows and decoders (before stream 0's decoder and LiStopConnection())
    void destroyWindows();

    // Whether the window belongs to this session (stream 0's included)
    bool ownsWindow(Uint32 windowId) const;

    // The stream shown in a window, or -1
    int streamForWindow(Uint32 windowId) const;

    // The size of a stream's video, for mouse positions
    bool getStreamSize(int stream, int& width, int& height) const;

private:
    struct StreamWindow {
        int stream = 0;
        ClientMonitor monitor;

        // From the stream's setup callback (connection thread)
        SDL_atomic_t setUp = {};
        int videoFormat = 0;
        int width = 0;
        int height = 0;
        int frameRate = 0;

        // From the stream status callback (library thread): 0 while running
        SDL_atomic_t stopState = {};

        SDL_Window* window = nullptr;
        Uint32 windowId = 0;
        int currentDisplayIndex = -1;
        IVideoDecoder* decoder = nullptr;
        SDL_mutex* decoderLock = nullptr;
        int flushingWindowEvents = 0;
        bool stopped = false;
    };

    StreamWindow* findWindow(Uint32 windowId) const;
    StreamWindow* windowForStream(int stream) const;
    int displayIndexFor(const ClientMonitor& monitor) const;
    void getWindowedSize(const StreamWindow& streamWindow, int& width, int& height) const;
    int bitrateKbps(int streams) const;
    void flushWindowEvents(StreamWindow& streamWindow);
    bool recreateDecoder(StreamWindow& streamWindow);
    void handleWindowEvent(StreamWindow& streamWindow, const SDL_WindowEvent& event);
    void closeStream(StreamWindow& streamWindow, int reason);
    bool keyboardFocusIsOurs() const;
    void checkMonitorsPresent();
    void showStatus(const QString& text);

    static QString reasonText(int reason);
    static void requestQuit();
    static int extraSetup(int videoFormat, int width, int height, int frameRate, void* context, int drFlags);
    static void streamStatus(int stream, int state, int reason);
    static void setHdrMode(int stream, bool enabled);
    static Uint32 clearStatusTimer(Uint32 interval, void* param);

    Session* m_Session;
    DisplayPlan m_Plan;
    QVector<StreamWindow*> m_Windows; // streams 1 .. n-1; fixed after the constructor
    int m_RequestedStreams = 1;
    bool m_HostSupportsMultiStream = false;
    int m_HostStreams = 1;
    QString m_HostReason;
    bool m_EventsEnabled = false; // library callbacks may push SDL events (guarded by the instance lock)
    QVector<Uint32> m_ClosedWindowIds;
    int m_StatusGeneration = 0;
    SDL_TimerID m_StatusTimer = 0;
    bool m_PendingStatus = false; // shown before stream 0's renderer existed
};

}
