// Tessera multistream (protocol/multistream.md): display list, bitrate split, and the extra streams' life cycle in a
// connection. The extra streams run copies of the upstream video path (TesseraVideoInstance.h).
#include "Limelight-internal.h"
#include "Rtsp.h"

#include <stdio.h>
#include <string.h>

// One copy of the video path per extra stream
typedef char TesseraInstanceCountCheck[(TESSERA_MAX_STREAMS == 4) ? 1 : -1];

TESSERA_STREAM_SLOT TesseraStreams[TESSERA_MAX_STREAMS];

static int configuredCount; // displays given to LiTesseraConfigure(), 0 if not configured
static int streamCount = 1; // streams in the current connection
static TESSERA_CALLBACKS tesseraCallbacks;

static const TESSERA_VIDEO_INSTANCE_OPS* const videoOps[TESSERA_MAX_STREAMS] = {
    NULL, &TesseraVideoOps1, &TesseraVideoOps2, &TesseraVideoOps3
};
static const TESSERA_DEPACKETIZER_INSTANCE_OPS* const depacketizerOps[TESSERA_MAX_STREAMS] = {
    NULL, &TesseraDepacketizerOps1, &TesseraDepacketizerOps2, &TesseraDepacketizerOps3
};

// Defined in RtspConnection.c
bool tesseraRtspSetupStream(PRTSP_MESSAGE response, char* target, int* error);
bool parseSdpAttributeToUInt(const char* payload, const char* name, uint32_t* val);

static bool isValidDisplay(const TESSERA_DISPLAY* display) {
    return display->keyLength >= 1 && display->keyLength <= TESSERA_MONITOR_KEY_MAX_BYTES &&
           display->width > 0 && display->height > 0 &&
           display->refreshNumerator > 0 && display->refreshDenominator > 0 &&
           display->x >= -TESSERA_MAX_LAYOUT_COORDINATE && display->x <= TESSERA_MAX_LAYOUT_COORDINATE &&
           display->y >= -TESSERA_MAX_LAYOUT_COORDINATE && display->y <= TESSERA_MAX_LAYOUT_COORDINATE &&
           (display->dpi == 0 || (display->dpi >= TESSERA_MIN_DPI && display->dpi <= TESSERA_MAX_DPI));
}

int LiTesseraFormatDisplayList(const TESSERA_DISPLAY* displays, int count, char* out, size_t outSize) {
    static const char hexDigits[] = "0123456789abcdef";
    size_t length = 0;

    if (displays == NULL || out == NULL || count < 1 || count > TESSERA_MAX_REQUESTED_DISPLAYS || outSize == 0) {
        return -1;
    }

    for (int i = 0; i < count; i++) {
        const TESSERA_DISPLAY* display = &displays[i];
        char fields[128];
        int fieldsLength;

        if (!isValidDisplay(display)) {
            return -1;
        }

        if (i > 0) {
            if (length + 1 >= outSize) {
                return -1;
            }
            out[length++] = TESSERA_DISPLAY_ENTRY_SEPARATOR;
        }

        if (length + 2 * (size_t)display->keyLength >= outSize) {
            return -1;
        }
        for (int j = 0; j < display->keyLength; j++) {
            out[length++] = hexDigits[display->key[j] >> 4];
            out[length++] = hexDigits[display->key[j] & 0xF];
        }

        fieldsLength = snprintf(fields, sizeof(fields), ".%d.%d.%d.%d.%d.%d.%d.%u",
                                display->width, display->height,
                                display->refreshNumerator, display->refreshDenominator,
                                display->x, display->y, display->dpi,
                                display->hdr ? TESSERA_DISPLAY_FLAG_HDR : 0u);
        if (fieldsLength < 0 || length + (size_t)fieldsLength >= outSize) {
            return -1;
        }
        memcpy(&out[length], fields, (size_t)fieldsLength);
        length += (size_t)fieldsLength;
    }

    if (length > TESSERA_DISPLAY_LIST_MAX_CHARS) {
        return -1;
    }
    out[length] = 0;
    return (int)length;
}

// Never 0, so every display gets a share.
static uint64_t pixelRate(const TESSERA_DISPLAY* display) {
    uint64_t rate;

    if (display->width <= 0 || display->height <= 0 || display->refreshNumerator <= 0 || display->refreshDenominator <= 0) {
        return 1;
    }
    rate = (uint64_t)display->width * (uint64_t)display->height * (uint64_t)display->refreshNumerator /
           (uint64_t)display->refreshDenominator;
    return rate > 0 ? rate : 1;
}

void LiTesseraSplitBitrate(int totalKbps, const TESSERA_DISPLAY* displays, int count, int* outKbps) {
    bool atFloor[TESSERA_MAX_REQUESTED_DISPLAYS] = { false };
    int assigned;

    if (displays == NULL || outKbps == NULL || count < 1 || count > TESSERA_MAX_REQUESTED_DISPLAYS) {
        return;
    }
    if (totalKbps < 0) {
        totalKbps = 0;
    }

    // Too little for the floor everywhere: split evenly.
    if ((int64_t)totalKbps < (int64_t)count * TESSERA_MIN_STREAM_BITRATE_KBPS) {
        for (int i = 0; i < count; i++) {
            outKbps[i] = totalKbps / count;
        }
        outKbps[0] += totalKbps - (totalKbps / count) * count;
        return;
    }

    // Proportional shares; any share below the floor is raised to it and the rest is shared again among the others.
    for (int round = 0; round < count; round++) {
        uint64_t remaining = (uint64_t)totalKbps;
        uint64_t rateSum = 0;
        bool changed = false;

        for (int i = 0; i < count; i++) {
            if (atFloor[i]) {
                remaining -= TESSERA_MIN_STREAM_BITRATE_KBPS;
            }
            else {
                rateSum += pixelRate(&displays[i]);
            }
        }

        for (int i = 0; i < count; i++) {
            if (atFloor[i]) {
                outKbps[i] = TESSERA_MIN_STREAM_BITRATE_KBPS;
            }
            else {
                outKbps[i] = (int)(remaining * pixelRate(&displays[i]) / rateSum);
                if (outKbps[i] < TESSERA_MIN_STREAM_BITRATE_KBPS) {
                    atFloor[i] = true;
                    changed = true;
                }
            }
        }

        if (!changed) {
            break;
        }
    }

    assigned = 0;
    for (int i = 0; i < count; i++) {
        if (outKbps[i] < TESSERA_MIN_STREAM_BITRATE_KBPS) {
            outKbps[i] = TESSERA_MIN_STREAM_BITRATE_KBPS;
        }
        assigned += outKbps[i];
    }
    outKbps[0] += totalKbps - assigned;
}

// Defaults for decoder callbacks a client leaves out
static int noopDrSetup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) { return 0; }
static void noopDrVoid(void) {}
static int noopDrSubmit(PDECODE_UNIT decodeUnit) { return DR_OK; }

static void reportStatus(int stream, int state, int reason) {
    if (tesseraCallbacks.streamStatus != NULL) {
        tesseraCallbacks.streamStatus(stream, state, reason);
    }
}

int LiTesseraConfigure(const TESSERA_DISPLAY* displays, int count,
                       const DECODER_RENDERER_CALLBACKS* extraDecoderCallbacks, void* const* extraRendererContexts,
                       const TESSERA_CALLBACKS* callbacks) {
    if (displays == NULL || count < 1 || count > TESSERA_MAX_STREAMS) {
        return -1;
    }
    for (int i = 0; i < count; i++) {
        if (!isValidDisplay(&displays[i])) {
            return -1;
        }
    }

    LiTesseraReset();
    for (int i = 0; i < count; i++) {
        PTESSERA_STREAM_SLOT slot = &TesseraStreams[i];

        slot->display = displays[i];
        if (i == 0) {
            continue;
        }
        if (extraDecoderCallbacks != NULL) {
            slot->callbacks = extraDecoderCallbacks[i - 1];
        }
        slot->callbacks.setup = slot->callbacks.setup ? slot->callbacks.setup : noopDrSetup;
        slot->callbacks.start = slot->callbacks.start ? slot->callbacks.start : noopDrVoid;
        slot->callbacks.stop = slot->callbacks.stop ? slot->callbacks.stop : noopDrVoid;
        slot->callbacks.cleanup = slot->callbacks.cleanup ? slot->callbacks.cleanup : noopDrVoid;
        slot->callbacks.submitDecodeUnit = slot->callbacks.submitDecodeUnit ? slot->callbacks.submitDecodeUnit : noopDrSubmit;
        slot->rendererContext = extraRendererContexts != NULL ? extraRendererContexts[i - 1] : NULL;
    }
    if (callbacks != NULL) {
        tesseraCallbacks = *callbacks;
    }
    configuredCount = count;
    return 0;
}

void LiTesseraReset(void) {
    memset(TesseraStreams, 0, sizeof(TesseraStreams));
    memset(&tesseraCallbacks, 0, sizeof(tesseraCallbacks));
    configuredCount = 0;
    streamCount = 1;
}

int LiTesseraGetStreamCount(void) {
    return streamCount;
}

// The format of an extra stream: the negotiated codec, 10-bit only if that display is HDR.
static int formatForStream(int negotiated, bool hdr) {
    if (hdr) {
        return negotiated;
    }
    switch (negotiated) {
    case VIDEO_FORMAT_H265_MAIN10:
        return VIDEO_FORMAT_H265;
    case VIDEO_FORMAT_H265_REXT10_444:
        return VIDEO_FORMAT_H265_REXT8_444;
    case VIDEO_FORMAT_AV1_MAIN10:
        return VIDEO_FORMAT_AV1_MAIN8;
    case VIDEO_FORMAT_AV1_HIGH10_444:
        return VIDEO_FORMAT_AV1_HIGH8_444;
    default:
        return negotiated;
    }
}

void tesseraParseDescribe(const char* payload) {
    uint32_t hostStreams;
    TESSERA_DISPLAY displays[TESSERA_MAX_STREAMS];
    int kbps[TESSERA_MAX_STREAMS];
    int count;

    streamCount = 1;
    if (configuredCount < 2) {
        return;
    }
    if (!parseSdpAttributeToUInt(payload, TESSERA_SDP_STREAMS, &hostStreams) || hostStreams < 2) {
        Limelog("Tessera: the host offers no extra streams; streaming one display\n");
        return;
    }

    // Every stream is encrypted, stream 0 included, so they all share one packet size (protocol/multistream.md section 4)
    if (!(EncryptionFeaturesSupported & SS_ENC_VIDEO)) {
        Limelog("Tessera: the host can't encrypt video; streaming one display\n");
        return;
    }
    StreamConfig.encryptionFlags |= ENCFLG_VIDEO;

    count = configuredCount < (int)hostStreams ? configuredCount : (int)hostStreams;
    for (int i = 0; i < count; i++) {
        displays[i] = TesseraStreams[i].display;
    }
    LiTesseraSplitBitrate((int)StreamConfig.bitrate, displays, count, kbps);
    for (int i = 0; i < count; i++) {
        TesseraStreams[i].bitrateKbps = kbps[i];
        TesseraStreams[i].videoFormat = formatForStream(NegotiatedVideoFormat, TesseraStreams[i].display.hdr);
    }
    streamCount = count;
    Limelog("Tessera: %d streams (host offers %u)\n", count, hostStreams);
}

int tesseraSetupVideoStreams(void) {
    for (int i = 1; i < streamCount; i++) {
        RTSP_MESSAGE response;
        char target[32];
        char* pingPayload;
        int error = -1;

        snprintf(target, sizeof(target), "streamid=video/%d/0", i);
        if (!tesseraRtspSetupStream(&response, target, &error)) {
            Limelog("RTSP SETUP %s request failed: %d\n", target, error);
            return error;
        }

        memset(&TesseraStreams[i].pingPayload, 0, sizeof(TesseraStreams[i].pingPayload));
        pingPayload = response.message.response.statusCode == 200 ? getOptionContent(response.options, "X-SS-Ping-Payload") : NULL;
        if (pingPayload == NULL || strlen(pingPayload) != sizeof(TesseraStreams[i].pingPayload.payload)) {
            // Without its own ping payload the host can't tell this stream apart; keep the streams before it.
            Limelog("Tessera: SETUP %s failed (%d); streaming %d displays\n", target, response.message.response.statusCode, i);
            freeMessage(&response);
            streamCount = i;
            break;
        }
        memcpy(TesseraStreams[i].pingPayload.payload, pingPayload, sizeof(TesseraStreams[i].pingPayload.payload));
        freeMessage(&response);
    }
    return 0;
}

int tesseraStreamBitrateKbps(int stream) {
    return streamCount >= 2 ? TesseraStreams[stream].bitrateKbps : (int)StreamConfig.bitrate;
}

static bool decoderSupportsRfi(const TESSERA_STREAM_SLOT* slot) {
    return ((slot->videoFormat & VIDEO_FORMAT_MASK_H264) && (slot->callbacks.capabilities & CAPABILITY_REFERENCE_FRAME_INVALIDATION_AVC)) ||
           ((slot->videoFormat & VIDEO_FORMAT_MASK_H265) && (slot->callbacks.capabilities & CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC)) ||
           ((slot->videoFormat & VIDEO_FORMAT_MASK_AV1) && (slot->callbacks.capabilities & CAPABILITY_REFERENCE_FRAME_INVALIDATION_AV1));
}

static int fpsOf(const TESSERA_DISPLAY* display) {
    return (display->refreshNumerator + display->refreshDenominator / 2) / display->refreshDenominator;
}

int tesseraAddSdpAttributes(int (*addAttribute)(void* head, char* name, const char* payload), void* head) {
    char name[64];
    char value[32];
    int err = 0;

    if (streamCount < 2) {
        return 0;
    }

#define TESSERA_ADD(nameFormat, valueFormat, valueArg)                \
    do {                                                              \
        snprintf(name, sizeof(name), nameFormat, i);                  \
        snprintf(value, sizeof(value), valueFormat, valueArg);        \
        err |= addAttribute(head, name, value);                       \
    } while (0)

    snprintf(value, sizeof(value), "%d", streamCount);
    err |= addAttribute(head, TESSERA_SDP_STREAMS, value);

    for (int i = 1; i < streamCount; i++) {
        const TESSERA_STREAM_SLOT* slot = &TesseraStreams[i];
        const TESSERA_DISPLAY* display = &slot->display;
        unsigned char slices = (unsigned char)(slot->callbacks.capabilities >> 24);
        // As for stream 0: 20% on top for FEC, and at most 100 Mbps
        int adjustedBitrate = (int)(slot->bitrateKbps * 0.80);

        adjustedBitrate = adjustedBitrate > 100000 ? 100000 : adjustedBitrate;
        TESSERA_ADD("x-nv-video[%d].clientViewportWd", "%d", display->width);
        TESSERA_ADD("x-nv-video[%d].clientViewportHt", "%d", display->height);
        TESSERA_ADD("x-nv-video[%d].maxFPS", "%d", fpsOf(display));
        TESSERA_ADD("x-nv-video[%d].clientRefreshRateX100", "%d",
                    (int)((int64_t)display->refreshNumerator * 100 / display->refreshDenominator));
        TESSERA_ADD("x-nv-video[%d].videoEncoderSlicesPerFrame", "%d", slices == 0 ? 1 : slices);
        TESSERA_ADD("x-nv-video[%d].dynamicRangeMode", "%d", (slot->videoFormat & VIDEO_FORMAT_MASK_10BIT) ? 1 : 0);
        TESSERA_ADD("x-nv-video[%d].maxNumReferenceFrames", "%d", decoderSupportsRfi(slot) ? 0 : 1);
        TESSERA_ADD("x-nv-vqos[%d].bw.maximumBitrateKbps", "%d", adjustedBitrate);
        TESSERA_ADD("x-ml-video[%d].configuredBitrateKbps", "%d", slot->bitrateKbps);
        TESSERA_ADD("x-nv-vqos[%d].fec.minRequiredFecPackets", "%d", 2);
    }

#undef TESSERA_ADD
    return err;
}

// Per-stream replacements for ListenerCallbacks.connectionTerminated: a failed extra stream stops alone.
static void streamTerminated(int stream, int errorCode) {
    PTESSERA_STREAM_SLOT slot = &TesseraStreams[stream];

    if (slot->stopped) {
        return;
    }
    slot->stopped = true;
    Limelog("Tessera: stream %d stopped (error %d)\n", stream, errorCode);
    reportStatus(stream, TESSERA_STREAM_STOPPED,
                 (errorCode == ML_ERROR_NO_VIDEO_TRAFFIC || errorCode == ML_ERROR_NO_VIDEO_FRAME) ? TESSERA_REASON_NO_VIDEO : TESSERA_REASON_NONE);
}

static void stream1Terminated(int errorCode) { streamTerminated(1, errorCode); }
static void stream2Terminated(int errorCode) { streamTerminated(2, errorCode); }
static void stream3Terminated(int errorCode) { streamTerminated(3, errorCode); }

static void (*const streamTerminators[TESSERA_MAX_STREAMS])(int) = {
    NULL, stream1Terminated, stream2Terminated, stream3Terminated
};

void tesseraInitializeVideoStreams(void) {
    for (int i = 1; i < streamCount; i++) {
        PTESSERA_STREAM_SLOT slot = &TesseraStreams[i];

        // Everything as for stream 0 (keys, packet size, colour space), except the display's own mode and bitrate
        slot->config = StreamConfig;
        slot->config.width = slot->display.width;
        slot->config.height = slot->display.height;
        slot->config.fps = fpsOf(&slot->display);
        slot->config.clientRefreshRateX100 = (int)((int64_t)slot->display.refreshNumerator * 100 / slot->display.refreshDenominator);
        slot->config.bitrate = slot->bitrateKbps;

        slot->listener = ListenerCallbacks;
        slot->listener.connectionTerminated = streamTerminators[i];
        slot->stopped = false;
        slot->hostHdrMode = false;

        videoOps[i]->initialize();
        slot->initialized = true;
    }
}

void tesseraStartVideoStreams(void* rendererContext, int drFlags) {
    (void)rendererContext; // stream 0's; the extra streams have their own

    for (int i = 1; i < streamCount; i++) {
        PTESSERA_STREAM_SLOT slot = &TesseraStreams[i];
        int err;

        if (!slot->initialized || slot->stopped) {
            continue;
        }
        err = videoOps[i]->start(slot->rendererContext, drFlags);
        if (err != 0) {
            Limelog("Tessera: stream %d failed to start: %d\n", i, err);
            slot->stopped = true;
            reportStatus(i, TESSERA_STREAM_STOPPED, TESSERA_REASON_DECODER_FAILED);
            continue;
        }
        slot->started = true;
        reportStatus(i, TESSERA_STREAM_RUNNING, TESSERA_REASON_NONE);
    }
}

void tesseraStopVideoStreams(void) {
    for (int i = 1; i < TESSERA_MAX_STREAMS; i++) {
        if (TesseraStreams[i].started) {
            // No callbacks once the connection is being stopped
            TesseraStreams[i].stopped = true;
            videoOps[i]->stop();
            TesseraStreams[i].started = false;
        }
    }
}

void tesseraDestroyVideoStreams(void) {
    for (int i = 1; i < TESSERA_MAX_STREAMS; i++) {
        if (TesseraStreams[i].initialized) {
            videoOps[i]->destroy();
            TesseraStreams[i].initialized = false;
        }
    }
    streamCount = 1;
}

static bool isExtraStream(int stream) {
    return stream >= 1 && stream < streamCount && TesseraStreams[stream].initialized;
}

bool LiTesseraWaitForNextVideoFrame(int stream, VIDEO_FRAME_HANDLE* frameHandle, PDECODE_UNIT* decodeUnit) {
    if (stream == 0) {
        return LiWaitForNextVideoFrame(frameHandle, decodeUnit);
    }
    return isExtraStream(stream) && depacketizerOps[stream]->waitForNextFrame(frameHandle, decodeUnit);
}

bool LiTesseraPollNextVideoFrame(int stream, VIDEO_FRAME_HANDLE* frameHandle, PDECODE_UNIT* decodeUnit) {
    if (stream == 0) {
        return LiPollNextVideoFrame(frameHandle, decodeUnit);
    }
    return isExtraStream(stream) && depacketizerOps[stream]->pollNextFrame(frameHandle, decodeUnit);
}

void LiTesseraWakeWaitForVideoFrame(int stream) {
    if (stream == 0) {
        LiWakeWaitForVideoFrame();
    }
    else if (isExtraStream(stream)) {
        depacketizerOps[stream]->wakeWaitForFrame();
    }
}

void LiTesseraCompleteVideoFrame(int stream, VIDEO_FRAME_HANDLE handle, int drStatus) {
    if (stream == 0) {
        LiCompleteVideoFrame(handle, drStatus);
    }
    else if (isExtraStream(stream)) {
        depacketizerOps[stream]->completeFrame(handle, drStatus);
    }
}

const RTP_VIDEO_STATS* LiTesseraGetRTPVideoStats(int stream) {
    if (stream == 0) {
        return LiGetRTPVideoStats();
    }
    return isExtraStream(stream) ? videoOps[stream]->getRtpStats() : NULL;
}

bool tesseraIsReferenceFrameInvalidationEnabled(int stream) {
    return ReferenceFrameInvalidationSupported && decoderSupportsRfi(&TesseraStreams[stream]);
}

bool tesseraGetHostHdrMode(int stream) {
    return TesseraStreams[stream].hostHdrMode;
}

void tesseraReportHostStreamStatus(int stream, int state, int reason) {
    if (!isExtraStream(stream)) {
        return;
    }
    if (state == TESSERA_STREAM_STOPPED) {
        TesseraStreams[stream].stopped = true;
    }
    reportStatus(stream, state, reason);
}

void tesseraSetHostHdrMode(int stream, bool enabled) {
    if (!isExtraStream(stream)) {
        return;
    }
    TesseraStreams[stream].hostHdrMode = enabled;
    if (tesseraCallbacks.setHdrMode != NULL) {
        tesseraCallbacks.setHdrMode(stream, enabled);
    }
}

int tesseraGetStreamCountInternal(void) {
    return streamCount;
}
