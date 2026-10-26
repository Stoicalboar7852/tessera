/**
 * @file host-helpers/tests/display_modes_test.cpp
 * @brief Unit tests for the virtual monitors' timings, mode lists and EDID bytes.
 */
// lib includes
#include <gtest/gtest.h>

// local includes
#include "tessera/display_modes.h"

using namespace tessera::modes;

namespace {

  std::uint32_t block_sum(const std::uint8_t *b) {
    std::uint32_t s = 0;
    for (int i = 0; i < 128; ++i) {
      s += b[i];
    }
    return s % 256;
  }

  edid_options_t options(bool hdr = false) {
    edid_options_t o;
    const std::uint8_t id[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    o.identity = identity_from_id(id);
    std::memcpy(o.name, "Desk-2", 7);
    o.hdr = hdr;
    return o;
  }

}  // namespace

TEST(DisplayModes, CvtRbMatchesTheVesaTimingFor1080p60) {
  const timing_t t = cvt_rb(1920, 1080, {60, 1});
  EXPECT_EQ(t.h_total(), 2080u);  // 160-pixel reduced horizontal blanking
  EXPECT_EQ(t.v_total(), 1111u);  // VESA DMT/CVT-RB 1920x1080@60: 1111 lines
  EXPECT_EQ(t.h_front, 48u);
  EXPECT_EQ(t.h_sync, 32u);
  EXPECT_EQ(t.v_front, 3u);
  EXPECT_EQ(t.v_sync, 5u);  // 16:9
  EXPECT_EQ(t.pixel_clock_hz, 2080ull * 1111ull * 60ull);
}

TEST(DisplayModes, FractionalRefreshStaysExact) {
  const timing_t t = cvt_rb(1920, 1080, {60000, 1001});
  EXPECT_EQ(t.refresh.numerator, 60000u);
  EXPECT_EQ(t.refresh.denominator, 1001u);
  EXPECT_EQ(refresh_mhz(t.refresh), 59940u);
  const timing_t u = cvt_rb(3840, 2160, {120000, 1001});
  EXPECT_EQ(refresh_mhz(u.refresh), 119880u);
  // The pixel clock reproduces the exact rate to well under 1 ppm.
  const double rate = static_cast<double>(u.pixel_clock_hz) / (static_cast<double>(u.h_total()) * u.v_total());
  EXPECT_NEAR(rate, 120000.0 / 1001.0, 1e-4);
}

TEST(DisplayModes, HorizontalTotalIsACellMultiple) {
  const timing_t t = cvt_rb(1366, 768, {60, 1});
  EXPECT_EQ(t.h_total() % 8, 0u);
  EXPECT_EQ(t.width, 1366u);
}

TEST(DisplayModes, VerticalBlankingIsAtLeast460Microseconds) {
  for (const std::uint32_t hz : {30u, 60u, 120u, 144u, 240u}) {
    const timing_t t = cvt_rb(2560, 1440, {hz, 1});
    const double line_us = 1e6 / hz / t.v_total();
    EXPECT_GE((t.v_total() - t.height) * line_us, 460.0) << hz << " Hz";
  }
}

TEST(DisplayModes, ModeListHasRequestedModeFirstAndNoDuplicates) {
  mode_list_t list;
  ASSERT_TRUE(build_mode_list(2560, 1600, {120, 1}, list));
  ASSERT_GE(list.count, 2u);
  ASSERT_LE(list.count, max_modes);
  EXPECT_EQ(list.modes[0].width, 2560u);
  EXPECT_EQ(list.modes[0].height, 1600u);
  EXPECT_EQ(refresh_mhz(list.modes[0].refresh), 120000u);
  for (std::uint32_t i = 0; i < list.count; ++i) {
    for (std::uint32_t j = i + 1; j < list.count; ++j) {
      const bool same = list.modes[i].width == list.modes[j].width && list.modes[i].height == list.modes[j].height && same_refresh(list.modes[i].refresh, list.modes[j].refresh);
      EXPECT_FALSE(same) << i << " and " << j;
    }
  }
  mode_list_t common;
  ASSERT_TRUE(build_mode_list(1920, 1080, {60, 1}, common));
  EXPECT_EQ(common.count, 4u);  // 1080p60 is also in the safe set, so only 720p, 1440p and 2160p are added
}

TEST(DisplayModes, ModeListRejectsOutOfRangeRequests) {
  mode_list_t list;
  EXPECT_FALSE(build_mode_list(320, 240, {60, 1}, list));
  EXPECT_FALSE(build_mode_list(1920, 1080, {10, 1}, list));
  EXPECT_FALSE(build_mode_list(1920, 1080, {600, 1}, list));
  EXPECT_FALSE(build_mode_list(1920, 1080, {60, 0}, list));
  EXPECT_FALSE(build_mode_list(9000, 1080, {60, 1}, list));
  EXPECT_EQ(list.count, 0u);
}

TEST(DisplayModes, SdrEdidIsAValidBaseBlock) {
  mode_list_t list;
  ASSERT_TRUE(build_mode_list(1920, 1080, {60, 1}, list));
  edid_t e;
  ASSERT_TRUE(build_edid(list, options(), e));
  ASSERT_EQ(e.size, 128u);
  const std::uint8_t header[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
  EXPECT_EQ(std::memcmp(e.bytes, header, 8), 0);
  EXPECT_EQ(block_sum(e.bytes), 0u);
  EXPECT_EQ(e.bytes[18], 1);
  EXPECT_EQ(e.bytes[19], 4);
  EXPECT_EQ(e.bytes[126], 0);  // no extension
  // Detailed timing 1 = preferred mode: 138.65 MHz, 1920 x 1080.
  EXPECT_EQ(e.bytes[54] | (e.bytes[55] << 8), 13865);
  EXPECT_EQ(e.bytes[56] | ((e.bytes[58] >> 4) << 8), 1920);
  EXPECT_EQ(e.bytes[59] | ((e.bytes[61] >> 4) << 8), 1080);
  // Name descriptor.
  EXPECT_EQ(e.bytes[75], 0xfc);
  EXPECT_EQ(std::memcmp(e.bytes + 77, "Desk-2\n", 7), 0);
  // About 96 DPI: 1920 px = 508 mm.
  EXPECT_EQ(e.bytes[21], 51);
}

TEST(DisplayModes, IdentityRoundTripsThroughTheEdid) {
  mode_list_t list;
  ASSERT_TRUE(build_mode_list(1920, 1080, {60, 1}, list));
  edid_t e;
  const edid_options_t o = options();
  ASSERT_TRUE(build_edid(list, o, e));
  edid_identity_t back;
  ASSERT_TRUE(read_identity(e.bytes, e.size, back));
  EXPECT_EQ(back, o.identity);
  // Another monitor ID gives another identity; the same ID always gives the same one.
  const std::uint8_t other[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 17};
  EXPECT_FALSE(identity_from_id(other) == o.identity);
  const std::uint8_t same[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  EXPECT_EQ(identity_from_id(same), o.identity);
  // Not a Tessera EDID.
  e.bytes[9] ^= 0x01;
  EXPECT_FALSE(read_identity(e.bytes, e.size, back));
  EXPECT_FALSE(read_identity(e.bytes, 64, back));
}

TEST(DisplayModes, HdrEdidHasCtaExtensionWithPqAndBt2020) {
  mode_list_t list;
  ASSERT_TRUE(build_mode_list(3840, 2160, {60, 1}, list));
  edid_t e;
  ASSERT_TRUE(build_edid(list, options(true), e));
  ASSERT_EQ(e.size, 256u);
  EXPECT_EQ(e.bytes[126], 1);
  EXPECT_EQ(block_sum(e.bytes), 0u);
  const std::uint8_t *x = e.bytes + 128;
  EXPECT_EQ(block_sum(x), 0u);
  EXPECT_EQ(x[0], 0x02);
  EXPECT_EQ(x[1], 0x03);
  // Colorimetry block (extended tag 5) with BT.2020 RGB, then HDR static metadata (extended tag 6) with PQ.
  EXPECT_EQ(x[4], 0xe3);
  EXPECT_EQ(x[5], 0x05);
  EXPECT_EQ(x[6] & 0x80, 0x80);
  EXPECT_EQ(x[8], 0xe6);
  EXPECT_EQ(x[9], 0x06);
  EXPECT_EQ(x[10] & 0x04, 0x04);
  EXPECT_EQ(x[2], 4 + 4 + 7);  // offset of the (absent) detailed timings
  EXPECT_EQ((e.bytes[20] >> 4) & 7, 3);  // 10 bits per colour
}

TEST(DisplayModes, FastModesStillGetAValidEdid) {
  // 3840x2160 at 240 Hz needs ~2.1 GHz, beyond an EDID detailed timing; the EDID then describes the 60 Hz variant.
  mode_list_t list;
  ASSERT_TRUE(build_mode_list(3840, 2160, {240, 1}, list));
  edid_t e;
  ASSERT_TRUE(build_edid(list, options(), e));
  EXPECT_EQ(block_sum(e.bytes), 0u);
  EXPECT_EQ(e.bytes[56] | ((e.bytes[58] >> 4) << 8), 3840);
}

TEST(DisplayModes, AsciiNameReplacesNonAscii) {
  const std::uint16_t name[] = {u'D', u'e', u's', u'k', u' ', 0x00e9, u'2', 0};
  char out[14];
  ascii_name(name, 32, out);
  EXPECT_STREQ(out, "Desk ?2");
  const std::uint16_t longname[] = {u'A', u'B', u'C', u'D', u'E', u'F', u'G', u'H', u'I', u'J', u'K', u'L', u'M', u'N', u'O', 0};
  ascii_name(longname, 32, out);
  EXPECT_STREQ(out, "ABCDEFGHIJKLM");
}
