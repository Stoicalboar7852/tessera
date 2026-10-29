// Tessera multistream: per-stream control messages and absolute mouse input (protocol/multistream.md sections 5, 6).
#include "Limelight-internal.h"

#include <string.h>

static PLT_MUTEX mouseMutex;
static bool mouseMutexCreated;
static struct {
    tessera_mouse_position_t latest;
    bool dirty; // a queued packet will pick up `latest` when the input thread sends it
} mouseState;

static void sendStreamMessage(unsigned short type, int stream) {
    tessera_ctrl_stream_t payload;

    memset(&payload, 0, sizeof(payload));
    payload.stream = (uint8_t)stream;
    tesseraSendControlMessage(type, &payload, sizeof(payload), CTRL_CHANNEL_URGENT);
}

void tesseraRequestIdrFrame(int stream) {
    if (TesseraStreams[stream].stopped) {
        return;
    }
    Limelog("Tessera: requesting IDR frame on stream %d\n", stream);
    sendStreamMessage(TESSERA_CTRL_IDR_REQUEST, stream);
}

void tesseraDetectedFrameLoss(int stream, uint32_t startFrame, uint32_t endFrame) {
    tessera_ctrl_invalidate_t payload;

    if (TesseraStreams[stream].stopped) {
        return;
    }
    if (!tesseraIsReferenceFrameInvalidationEnabled(stream)) {
        tesseraRequestIdrFrame(stream);
        return;
    }

    memset(&payload, 0, sizeof(payload));
    payload.header.stream = (uint8_t)stream;
    payload.first_frame = (int64_t)LE64((uint64_t)startFrame);
    payload.last_frame = (int64_t)LE64((uint64_t)endFrame);
    Limelog("Tessera: invalidating reference frames %u to %u on stream %d\n", startFrame, endFrame, stream);
    tesseraSendControlMessage(TESSERA_CTRL_INVALIDATE_REF_FRAMES, &payload, sizeof(payload), CTRL_CHANNEL_URGENT);
}

// Stream 0 uses these for LTR acknowledgements and loss statistics; multistream v1 has neither for extra streams.
void tesseraReceivedCompleteFrame(int stream, uint32_t frameIndex, bool frameIsLTR) {}
void tesseraSawFrame(int stream, uint32_t frameIndex) {}
void tesseraSendFrameFecStatus(int stream, PSS_FRAME_FEC_STATUS fecStatus) {}

void tesseraHandleControlMessage(unsigned short type, const uint8_t* payload, int length) {
    if (type == TESSERA_CTRL_HDR_MODE) {
        // tessera_ctrl_stream_t, then the legacy HDR mode payload: enabled byte + SS_HDR_METADATA (not used per stream yet)
        if (length >= (int)sizeof(tessera_ctrl_stream_t) + 1) {
            tesseraSetHostHdrMode(payload[0], payload[sizeof(tessera_ctrl_stream_t)] != 0);
        }
    }
    else if (type == TESSERA_CTRL_STREAM_STATUS) {
        if (length >= (int)sizeof(tessera_ctrl_stream_status_t)) {
            const tessera_ctrl_stream_status_t* status = (const tessera_ctrl_stream_status_t*)payload;

            Limelog("Tessera: host reports stream %u %s (reason %u)\n", status->header.stream,
                    status->state == TESSERA_STREAM_RUNNING ? "running" : "stopped", status->reason);
            tesseraReportHostStreamStatus(status->header.stream, status->state, status->reason);
        }
    }
}

int LiTesseraSendMousePosition(int stream, short x, short y, short referenceWidth, short referenceHeight) {
    tessera_mouse_position_t packet;
    int err;

    if (tesseraGetStreamCountInternal() < 2) {
        return stream == 0 ? LiSendMousePositionEvent(x, y, referenceWidth, referenceHeight) : -1;
    }
    if (stream < 0 || stream >= tesseraGetStreamCountInternal()) {
        return -1;
    }
    if (!mouseMutexCreated) {
        // First use is from the client's input thread, before any concurrent call
        if (PltCreateMutex(&mouseMutex) != 0) {
            return -1;
        }
        mouseMutexCreated = true;
    }

    PltLockMutex(&mouseMutex);
    mouseState.latest.stream = (uint8_t)stream;
    mouseState.latest.x = x;
    mouseState.latest.y = y;
    mouseState.latest.width = referenceWidth;
    mouseState.latest.height = referenceHeight;
    if (mouseState.dirty) {
        // The queued packet will carry this position
        PltUnlockMutex(&mouseMutex);
        return 0;
    }
    mouseState.dirty = true;
    PltUnlockMutex(&mouseMutex);

    memset(&packet, 0, sizeof(packet));
    packet.size = BE32((uint32_t)(sizeof(packet) - sizeof(packet.size)));
    packet.magic = LE32(TESSERA_MOUSE_POSITION_MAGIC);
    err = tesseraQueueInputPacket(&packet, sizeof(packet), CTRL_CHANNEL_MOUSE);
    if (err != 0) {
        PltLockMutex(&mouseMutex);
        mouseState.dirty = false;
        PltUnlockMutex(&mouseMutex);
    }
    return err;
}

void tesseraFillMousePosition(NV_INPUT_HEADER* header) {
    tessera_mouse_position_t* packet = (tessera_mouse_position_t*)header;

    PltLockMutex(&mouseMutex);
    packet->stream = mouseState.latest.stream;
    packet->x = (int16_t)BE16((uint16_t)mouseState.latest.x);
    packet->y = (int16_t)BE16((uint16_t)mouseState.latest.y);
    packet->width = (int16_t)BE16((uint16_t)mouseState.latest.width);
    packet->height = (int16_t)BE16((uint16_t)mouseState.latest.height);
    mouseState.dirty = false;
    PltUnlockMutex(&mouseMutex);
}
