// Tessera extensions to moonlight-common-c (protocol/multistream.md, docs/adr/0007-multistream-protocol.md).
// Everything here is additive: a client that never calls these functions behaves exactly like upstream.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Limelight.h"
#include "../../protocol/include/tessera/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

// One client monitor, as reported to the host in the launch request.
typedef struct _TESSERA_DISPLAY {
    // Stable per client monitor (for example a hash of the OS monitor ID), 1 to TESSERA_MONITOR_KEY_MAX_BYTES bytes
    uint8_t key[TESSERA_MONITOR_KEY_MAX_BYTES];
    int keyLength;

    int width;
    int height;

    // Refresh rate as an exact fraction of hertz (60000/1001 for 59.94 Hz)
    int refreshNumerator;
    int refreshDenominator;

    // Position in the client's monitor layout, in the same pixel units as width and height
    int x;
    int y;

    // 0 if unknown
    int dpi;

    bool hdr;
} TESSERA_DISPLAY, *PTESSERA_DISPLAY;

// Writes the display list for the launch request's "tmsd" argument (protocol/multistream.md section 2), NUL-terminated.
// Display 0 must be the primary monitor, matching the launch "mode". Returns the length without the NUL, or -1 if a
// display is invalid, there are more than TESSERA_MAX_REQUESTED_DISPLAYS, or the result doesn't fit in outSize.
int LiTesseraFormatDisplayList(const TESSERA_DISPLAY* displays, int count, char* out, size_t outSize);

// Splits a total bitrate across the displays by pixel rate (width x height x refresh), giving each at least
// TESSERA_MIN_STREAM_BITRATE_KBPS where the total allows. Rounding leftovers go to display 0. The results add up to
// totalKbps.
void LiTesseraSplitBitrate(int totalKbps, const TESSERA_DISPLAY* displays, int count, int* outKbps);

// Notifications about extra streams. They run on library threads and must not block.
typedef struct _TESSERA_CALLBACKS {
    // An extra stream started (TESSERA_STREAM_RUNNING) or stopped (TESSERA_STREAM_STOPPED, with an
    // enum tessera_reason_e). The other streams carry on.
    void (*streamStatus)(int stream, int state, int reason);

    // The host turned HDR on or off for an extra stream (stream 0 uses CONNECTION_LISTENER_CALLBACKS.setHdrMode).
    void (*setHdrMode)(int stream, bool enabled);
} TESSERA_CALLBACKS, *PTESSERA_CALLBACKS;

// Call before LiStartConnection() to stream several displays. displays[0] is the primary monitor: it is stream 0 and
// uses the STREAM_CONFIGURATION and decoder callbacks given to LiStartConnection(). Display n (1 .. count-1) uses
// extraDecoderCallbacks[n-1] and extraRendererContexts[n-1] (either array may be NULL; missing callbacks do nothing).
// The configured bitrate is split across the streams the host accepts. count is 1 to TESSERA_MAX_STREAMS; 1 turns
// multistream off. Returns 0, or -1 if the arguments are invalid. The configuration stays until LiTesseraReset().
int LiTesseraConfigure(const TESSERA_DISPLAY* displays, int count,
                       const DECODER_RENDERER_CALLBACKS* extraDecoderCallbacks, void* const* extraRendererContexts,
                       const TESSERA_CALLBACKS* callbacks);

// Back to a plain single-stream client.
void LiTesseraReset(void);

// Streams in the current connection: 1 until the host has accepted more (after LiStartConnection() returns).
int LiTesseraGetStreamCount(void);

// Per-stream versions of the pull-renderer and statistics functions in Limelight.h (stream 0 included).
bool LiTesseraWaitForNextVideoFrame(int stream, VIDEO_FRAME_HANDLE* frameHandle, PDECODE_UNIT* decodeUnit);
bool LiTesseraPollNextVideoFrame(int stream, VIDEO_FRAME_HANDLE* frameHandle, PDECODE_UNIT* decodeUnit);
void LiTesseraWakeWaitForVideoFrame(int stream);
void LiTesseraCompleteVideoFrame(int stream, VIDEO_FRAME_HANDLE handle, int drStatus);
const RTP_VIDEO_STATS* LiTesseraGetRTPVideoStats(int stream);

// Absolute mouse position on one stream's display: x and y in 0 .. referenceWidth/Height-1, where the reference is
// the client's view of that stream. With a single stream this is LiSendMousePositionEvent().
int LiTesseraSendMousePosition(int stream, short x, short y, short referenceWidth, short referenceHeight);

#ifdef __cplusplus
}
#endif
