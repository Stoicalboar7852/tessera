#include "videostats.h"

#include <Tessera.h>

#include <cstdio>
#include <cstring>
#include <mutex>

namespace tessera {

namespace {

constexpr int kReportsPerLog = 10;
constexpr uint64_t kOverlayLineMaxAgeUs = 3000000;

struct StreamTotals {
    int reports;
    double seconds;
    uint64_t received;
    uint64_t decoded;
    uint64_t rendered;
    uint64_t total;
    uint64_t networkDropped;
    uint64_t pacerDropped;
    uint64_t decodeUs;
    uint64_t renderUs;
    char overlayLine[192];
    uint64_t overlayLineUs;
};

std::mutex s_Lock;
StreamTotals s_Streams[TESSERA_MAX_STREAMS];

double perFrameMs(uint64_t totalUs, uint64_t frames)
{
    return frames != 0 ? totalUs / 1000.0 / frames : 0.0;
}

}

void reportVideoStats(int stream, const VIDEO_STATS& lastSecond, int width, int height)
{
    if (stream < 0 || stream >= TESSERA_MAX_STREAMS) {
        return;
    }

    const uint64_t now = LiGetMicroseconds();
    const double seconds = lastSecond.measurementStartUs != 0 && now > lastSecond.measurementStartUs ?
                               (now - lastSecond.measurementStartUs) / 1000000.0 : 0.0;
    char logLine[384] = {};
    {
        std::lock_guard<std::mutex> guard(s_Lock);
        StreamTotals& totals = s_Streams[stream];

        totals.reports++;
        totals.seconds += seconds;
        totals.received += lastSecond.receivedFrames;
        totals.decoded += lastSecond.decodedFrames;
        totals.rendered += lastSecond.renderedFrames;
        totals.total += lastSecond.totalFrames;
        totals.networkDropped += lastSecond.networkDroppedFrames;
        totals.pacerDropped += lastSecond.pacerDroppedFrames;
        totals.decodeUs += lastSecond.totalDecodeTimeUs;
        totals.renderUs += lastSecond.totalRenderTimeUs;

        if (stream > 0) {
            snprintf(totals.overlayLine, sizeof(totals.overlayLine),
                     "Display %d: %dx%d, %.1f/%.1f/%.1f FPS (network/decoded/rendered), decode %.2f ms, render %.2f ms, "
                     "dropped %u/%u\n",
                     stream, width, height, lastSecond.receivedFps, lastSecond.decodedFps, lastSecond.renderedFps,
                     perFrameMs(lastSecond.totalDecodeTimeUs, lastSecond.decodedFrames),
                     perFrameMs(lastSecond.totalRenderTimeUs, lastSecond.renderedFrames),
                     lastSecond.networkDroppedFrames, lastSecond.pacerDroppedFrames);
            totals.overlayLineUs = now;
        }

        if (totals.reports >= kReportsPerLog) {
            snprintf(logLine, sizeof(logLine),
                     "Tessera stats: stream=%d size=%dx%d seconds=%.1f received=%llu decoded=%llu rendered=%llu "
                     "total=%llu network_dropped=%llu pacer_dropped=%llu received_fps=%.2f rendered_fps=%.2f "
                     "decode_ms=%.2f render_ms=%.2f",
                     stream, width, height, totals.seconds,
                     (unsigned long long)totals.received, (unsigned long long)totals.decoded,
                     (unsigned long long)totals.rendered, (unsigned long long)totals.total,
                     (unsigned long long)totals.networkDropped, (unsigned long long)totals.pacerDropped,
                     totals.seconds > 0 ? totals.received / totals.seconds : 0.0,
                     totals.seconds > 0 ? totals.rendered / totals.seconds : 0.0,
                     perFrameMs(totals.decodeUs, totals.decoded), perFrameMs(totals.renderUs, totals.rendered));

            const uint64_t overlayLineUs = totals.overlayLineUs;
            char overlayLine[sizeof(totals.overlayLine)];
            memcpy(overlayLine, totals.overlayLine, sizeof(overlayLine));
            totals = {};
            totals.overlayLineUs = overlayLineUs;
            memcpy(totals.overlayLine, overlayLine, sizeof(overlayLine));
        }
    }

    if (logLine[0] != '\0') {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "%s", logLine);
    }
}

void appendOverlayLines(char* text, int maxLength)
{
    const uint64_t now = LiGetMicroseconds();
    std::lock_guard<std::mutex> guard(s_Lock);

    const size_t size = maxLength > 0 ? static_cast<size_t>(maxLength) : 0;
    size_t length = strlen(text);
    for (int stream = 1; stream < TESSERA_MAX_STREAMS && length + 1 < size; stream++) {
        const StreamTotals& totals = s_Streams[stream];
        if (totals.overlayLineUs == 0 || now - totals.overlayLineUs > kOverlayLineMaxAgeUs) {
            continue;
        }
        const int written = snprintf(text + length, size - length, "%s", totals.overlayLine);
        if (written < 0 || static_cast<size_t>(written) >= size - length) {
            text[length] = '\0';
            break;
        }
        length += written;
    }
}

void resetVideoStats()
{
    std::lock_guard<std::mutex> guard(s_Lock);
    for (StreamTotals& totals : s_Streams) {
        totals = {};
    }
}

void clearVideoStats(int stream)
{
    if (stream < 0 || stream >= TESSERA_MAX_STREAMS) {
        return;
    }
    std::lock_guard<std::mutex> guard(s_Lock);
    s_Streams[stream] = {};
}

}
