// Tessera multistream: display list and bitrate split (protocol/multistream.md).
#include "Tessera.h"

#include <stdio.h>
#include <string.h>

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
