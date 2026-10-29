// Tessera extensions to moonlight-common-c (protocol/multistream.md, docs/adr/0007-multistream-protocol.md).
// Everything here is additive: a client that never calls these functions behaves exactly like upstream.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

#ifdef __cplusplus
}
#endif
