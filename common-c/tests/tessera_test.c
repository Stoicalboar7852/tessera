// Tests for Tessera's additions to moonlight-common-c. Exits with the number of failed checks.
#include "Limelight-internal.h"
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

// Collects SDP attributes from tesseraAddSdpAttributes()
static struct {
    char names[64][64];
    char values[64][32];
    int count;
} sdp;

static int collectAttribute(void* head, char* name, const char* payload) {
    (void)head;
    if (sdp.count >= 64) {
        return -1;
    }
    snprintf(sdp.names[sdp.count], sizeof(sdp.names[0]), "%s", name);
    snprintf(sdp.values[sdp.count], sizeof(sdp.values[0]), "%s", payload);
    sdp.count++;
    return 0;
}

static const char* sdpValue(const char* name) {
    for (int i = 0; i < sdp.count; i++) {
        if (strcmp(sdp.names[i], name) == 0) {
            return sdp.values[i];
        }
    }
    return "";
}

static void configureTwoDisplays(bool secondHdr) {
    TESSERA_DISPLAY displays[2];

    displays[0] = makeDisplay(1, 2560, 1440, 60, 1);
    displays[1] = makeDisplay(2, 1920, 1080, 60, 1);
    displays[1].x = 2560;
    displays[1].hdr = secondHdr;
    CHECK(LiTesseraConfigure(displays, 2, NULL, NULL, NULL) == 0);

    memset(&StreamConfig, 0, sizeof(StreamConfig));
    StreamConfig.bitrate = TESSERA_TV_SPLIT_TOTAL_KBPS;
    EncryptionFeaturesSupported = SS_ENC_CONTROL_V2 | SS_ENC_VIDEO;
    NegotiatedVideoFormat = VIDEO_FORMAT_H265_MAIN10;
}

static void testNegotiatesStreamsFromDescribe(void) {
    // A host without the extension
    configureTwoDisplays(false);
    tesseraParseDescribe("a=x-ss-general.featureFlags:3\r\n");
    CHECK(LiTesseraGetStreamCount() == 1);
    CHECK(tesseraStreamBitrateKbps(0) == TESSERA_TV_SPLIT_TOTAL_KBPS);

    // A host that can't encrypt video can't run extra streams
    configureTwoDisplays(false);
    EncryptionFeaturesSupported = SS_ENC_CONTROL_V2;
    tesseraParseDescribe("a=x-tessera.streams:4\r\n");
    CHECK(LiTesseraGetStreamCount() == 1);

    // Two configured, four offered: two, stream 0 is encrypted too, and the bitrate is split
    configureTwoDisplays(false);
    tesseraParseDescribe("a=x-tessera.streams:4\r\n");
    CHECK(LiTesseraGetStreamCount() == 2);
    CHECK((StreamConfig.encryptionFlags & ENCFLG_VIDEO) != 0);
    CHECK(tesseraStreamBitrateKbps(0) == TESSERA_TV_SPLIT_KBPS_0);
    CHECK(tesseraStreamBitrateKbps(1) == TESSERA_TV_SPLIT_KBPS_1);

    // An SDR display gets the 8-bit format of the negotiated codec, an HDR one keeps 10-bit
    sdp.count = 0;
    CHECK(tesseraAddSdpAttributes(collectAttribute, NULL) == 0);
    CHECK(strcmp(sdpValue("x-tessera.streams"), "2") == 0);
    CHECK(strcmp(sdpValue("x-nv-video[1].clientViewportWd"), "1920") == 0);
    CHECK(strcmp(sdpValue("x-nv-video[1].clientViewportHt"), "1080") == 0);
    CHECK(strcmp(sdpValue("x-nv-video[1].maxFPS"), "60") == 0);
    CHECK(strcmp(sdpValue("x-nv-video[1].clientRefreshRateX100"), "6000") == 0);
    CHECK(strcmp(sdpValue("x-nv-video[1].dynamicRangeMode"), "0") == 0);
    CHECK(strcmp(sdpValue("x-nv-video[1].maxNumReferenceFrames"), "1") == 0);
    CHECK(strcmp(sdpValue("x-nv-vqos[1].bw.maximumBitrateKbps"), "14400") == 0); // 80% of 18000
    CHECK(strcmp(sdpValue("x-ml-video[1].configuredBitrateKbps"), "18000") == 0);
    CHECK(strcmp(sdpValue("x-nv-video[2].clientViewportWd"), "") == 0);

    configureTwoDisplays(true);
    tesseraParseDescribe("a=x-tessera.streams:2\r\n");
    sdp.count = 0;
    CHECK(tesseraAddSdpAttributes(collectAttribute, NULL) == 0);
    CHECK(strcmp(sdpValue("x-nv-video[1].dynamicRangeMode"), "1") == 0);

    // Fewer offered than configured
    {
        TESSERA_DISPLAY displays[3];

        displays[0] = makeDisplay(1, 1920, 1080, 60, 1);
        displays[1] = makeDisplay(2, 1920, 1080, 60, 1);
        displays[2] = makeDisplay(3, 1920, 1080, 60, 1);
        CHECK(LiTesseraConfigure(displays, 3, NULL, NULL, NULL) == 0);
        StreamConfig.bitrate = 30000;
        EncryptionFeaturesSupported = SS_ENC_VIDEO;
        tesseraParseDescribe("a=x-tessera.streams:2\r\n");
        CHECK(LiTesseraGetStreamCount() == 2);
        CHECK(tesseraStreamBitrateKbps(0) == 15000);
    }

    // Single display: multistream stays off
    {
        TESSERA_DISPLAY display = makeDisplay(1, 1920, 1080, 60, 1);

        CHECK(LiTesseraConfigure(&display, 1, NULL, NULL, NULL) == 0);
        tesseraParseDescribe("a=x-tessera.streams:4\r\n");
        CHECK(LiTesseraGetStreamCount() == 1);
        sdp.count = 0;
        CHECK(tesseraAddSdpAttributes(collectAttribute, NULL) == 0);
        CHECK(sdp.count == 0);
    }

    CHECK(LiTesseraConfigure(NULL, 1, NULL, NULL, NULL) == -1);
    LiTesseraReset();
}

int main(void) {
    testFormatsTheSharedTestVector();
    testRejectsInvalidDisplayLists();
    testSplitsTheBitrateByPixelRate();
    testNegotiatesStreamsFromDescribe();

    if (failures == 0) {
        printf("tessera_common_c_tests: all checks passed\n");
    }
    return failures;
}
