// Tests for Tessera's additions to moonlight-common-c. Exits with the number of failed checks.
#include "Tessera.h"
#include "multistream_vectors.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                             \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static TESSERA_DISPLAY makeDisplay(uint32_t key, int width, int height, int numerator, int denominator) {
    TESSERA_DISPLAY display;

    memset(&display, 0, sizeof(display));
    display.key[0] = (uint8_t)(key >> 24);
    display.key[1] = (uint8_t)(key >> 16);
    display.key[2] = (uint8_t)(key >> 8);
    display.key[3] = (uint8_t)key;
    display.keyLength = 4;
    display.width = width;
    display.height = height;
    display.refreshNumerator = numerator;
    display.refreshDenominator = denominator;
    return display;
}

static void testFormatsTheSharedTestVector(void) {
    TESSERA_DISPLAY displays[2];
    char out[TESSERA_DISPLAY_LIST_MAX_CHARS + 1];
    int length;

    displays[0] = makeDisplay(0xa1b2c3d4, 2560, 1440, 60, 1);
    displays[0].dpi = 109;
    displays[1] = makeDisplay(0xe5f60718, 1920, 1080, 60000, 1001);
    displays[1].x = 2560;
    displays[1].y = 180;
    displays[1].dpi = 92;
    displays[1].hdr = true;

    length = LiTesseraFormatDisplayList(displays, 2, out, sizeof(out));
    CHECK(length == (int)strlen(TESSERA_TV_DISPLAY_LIST_2));
    CHECK(strcmp(out, TESSERA_TV_DISPLAY_LIST_2) == 0);
}

static void testRejectsInvalidDisplayLists(void) {
    TESSERA_DISPLAY displays[TESSERA_MAX_REQUESTED_DISPLAYS + 1];
    char out[TESSERA_DISPLAY_LIST_MAX_CHARS + 1];
    char small[16];

    for (int i = 0; i <= TESSERA_MAX_REQUESTED_DISPLAYS; i++) {
        displays[i] = makeDisplay((uint32_t)i, 1920, 1080, 60, 1);
        displays[i].x = 1920 * i;
    }

    CHECK(LiTesseraFormatDisplayList(displays, 0, out, sizeof(out)) == -1);
    CHECK(LiTesseraFormatDisplayList(displays, TESSERA_MAX_REQUESTED_DISPLAYS + 1, out, sizeof(out)) == -1);
    CHECK(LiTesseraFormatDisplayList(displays, 1, small, sizeof(small)) == -1);
    CHECK(LiTesseraFormatDisplayList(displays, TESSERA_MAX_REQUESTED_DISPLAYS, out, sizeof(out)) > 0);

    displays[0].keyLength = 0;
    CHECK(LiTesseraFormatDisplayList(displays, 1, out, sizeof(out)) == -1);
    displays[0].keyLength = 4;
    displays[0].dpi = 20;
    CHECK(LiTesseraFormatDisplayList(displays, 1, out, sizeof(out)) == -1);
    displays[0].dpi = 0;
    displays[0].x = -40000;
    CHECK(LiTesseraFormatDisplayList(displays, 1, out, sizeof(out)) == -1);
    displays[0].x = -1920;
    CHECK(LiTesseraFormatDisplayList(displays, 1, out, sizeof(out)) > 0);
    CHECK(strcmp(out, "00000000.1920.1080.60.1.-1920.0.0.0") == 0);
}

static void testSplitsTheBitrateByPixelRate(void) {
    TESSERA_DISPLAY displays[3];
    int kbps[3];

    displays[0] = makeDisplay(1, 2560, 1440, 60, 1);
    displays[1] = makeDisplay(2, 1920, 1080, 60, 1);
    LiTesseraSplitBitrate(TESSERA_TV_SPLIT_TOTAL_KBPS, displays, 2, kbps);
    CHECK(kbps[0] == TESSERA_TV_SPLIT_KBPS_0);
    CHECK(kbps[1] == TESSERA_TV_SPLIT_KBPS_1);

    // A 59.94 Hz display gets a little less than a 60 Hz one; the sum stays exact.
    displays[1] = makeDisplay(2, 1920, 1080, 60000, 1001);
    LiTesseraSplitBitrate(TESSERA_TV_SPLIT_TOTAL_KBPS, displays, 2, kbps);
    CHECK(kbps[1] < TESSERA_TV_SPLIT_KBPS_1);
    CHECK(kbps[0] + kbps[1] == TESSERA_TV_SPLIT_TOTAL_KBPS);

    // A tiny display is raised to the floor and the rest is shared by the others.
    displays[1] = makeDisplay(2, 1920, 1080, 60, 1);
    displays[2] = makeDisplay(3, 640, 480, 24, 1);
    LiTesseraSplitBitrate(20000, displays, 3, kbps);
    CHECK(kbps[2] == TESSERA_MIN_STREAM_BITRATE_KBPS);
    CHECK(kbps[0] + kbps[1] + kbps[2] == 20000);
    CHECK(kbps[0] > kbps[1]);

    // Below the floor for everyone: an even split.
    LiTesseraSplitBitrate(2000, displays, 3, kbps);
    CHECK(kbps[0] == 668 && kbps[1] == 666 && kbps[2] == 666);
}

int main(void) {
    testFormatsTheSharedTestVector();
    testRejectsInvalidDisplayLists();
    testSplitsTheBitrateByPixelRate();

    if (failures == 0) {
        printf("tessera_common_c_tests: all checks passed\n");
    }
    return failures;
}
