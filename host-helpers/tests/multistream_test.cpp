/**
 * @file host-helpers/tests/multistream_test.cpp
 * @brief Unit tests for the host side of the multistream extension.
 */
// lib includes
#include <gtest/gtest.h>

// local includes
#include "multistream_vectors.h"
#include "tessera/multistream.h"

using namespace tessera::multistream;

namespace {

  client_display_t display(std::uint8_t key, std::uint32_t width, std::uint32_t height, std::int32_t x, std::int32_t y) {
    client_display_t d;
    d.key = {key};
    d.width = width;
    d.height = height;
    d.refresh = {60, 1};
    d.x = x;
    d.y = y;
    return d;
  }

  bool rejects(std::string_view list) {
    return !parse_display_list(list).has_value();
  }

}  // namespace

TEST(Multistream, ParsesTheSharedTestVector) {
  const auto displays = parse_display_list(TESSERA_TV_DISPLAY_LIST_2);
  ASSERT_TRUE(displays.has_value()) << displays.error().to_string();
  ASSERT_EQ(displays->size(), 2u);

  const auto &a = (*displays)[0];
  EXPECT_EQ(a.key, (std::vector<std::uint8_t> {0xa1, 0xb2, 0xc3, 0xd4}));
  EXPECT_EQ(a.width, 2560u);
  EXPECT_EQ(a.height, 1440u);
  EXPECT_EQ(a.refresh.numerator, 60u);
  EXPECT_EQ(a.refresh.denominator, 1u);
  EXPECT_EQ(a.dpi, 109u);
  EXPECT_FALSE(a.hdr);

  const auto &b = (*displays)[1];
  EXPECT_EQ(b.refresh.numerator, 60000u);
  EXPECT_EQ(b.refresh.denominator, 1001u);
  EXPECT_EQ(b.x, 2560);
  EXPECT_EQ(b.y, 180);
  EXPECT_TRUE(b.hdr);

  EXPECT_EQ(format_display_list(*displays), TESSERA_TV_DISPLAY_LIST_2);
}

TEST(Multistream, AcceptsNegativePositionsAndUnknownFlags) {
  const auto displays = parse_display_list("00ff.1920.1080.60.1.-1920.-200.0.6");
  ASSERT_TRUE(displays.has_value());
  EXPECT_EQ((*displays)[0].x, -1920);
  EXPECT_EQ((*displays)[0].y, -200);
  EXPECT_EQ((*displays)[0].dpi, 0u);
  EXPECT_FALSE((*displays)[0].hdr);  // flags 6: bit 0 clear, other bits ignored
}

TEST(Multistream, RejectsMalformedLists) {
  EXPECT_TRUE(rejects(""));
  EXPECT_TRUE(rejects("a1.1920.1080.60.1.0.0.96"));  // 8 fields
  EXPECT_TRUE(rejects("a1.1920.1080.60.1.0.0.96.0.0"));  // 10 fields
  EXPECT_TRUE(rejects("a.1920.1080.60.1.0.0.96.0"));  // odd-length key
  EXPECT_TRUE(rejects("zz.1920.1080.60.1.0.0.96.0"));  // not hex
  EXPECT_TRUE(rejects(std::string(66, 'a') + ".1920.1080.60.1.0.0.96.0"));  // key longer than 32 bytes
  EXPECT_TRUE(rejects("a1.320.240.60.1.0.0.96.0"));  // too small
  EXPECT_TRUE(rejects("a1.1920.1080.60.0.0.0.96.0"));  // zero denominator
  EXPECT_TRUE(rejects("a1.1920.1080.1000.1.0.0.96.0"));  // 1000 Hz
  EXPECT_TRUE(rejects("a1.1920.1080.60.1.40000.0.96.0"));  // position out of range
  EXPECT_TRUE(rejects("a1.1920.1080.60.1.+5.0.96.0"));  // '+' sign
  EXPECT_TRUE(rejects("a1.1920.1080.60.1.0.0.20.0"));  // DPI too low
  EXPECT_TRUE(rejects("a1.1920.1080.60.1.0.0.96.70000"));  // flags too large
  EXPECT_TRUE(rejects("a1.1920.1080.60.1.0.0.96.0~a1.1920.1080.60.1.1920.0.96.0"));  // repeated key
  EXPECT_TRUE(rejects("a1.1920.1080.60.1.0.0.96.0~"));  // empty entry
  EXPECT_TRUE(rejects("a1.19200000000.1080.60.1.0.0.96.0"));  // 11 digits

  std::string many;
  for (int i = 0; i < 17; ++i) {
    many += (i ? "~" : "") + std::string(i < 16 ? "0" : "1") + "0123456789abcdef"[i % 16] + ".1920.1080.60.1.0.0.96.0";
  }
  EXPECT_TRUE(rejects(many));  // 17 displays
}

TEST(Multistream, MatchesTheLaunchMode) {
  auto d = display(1, 1920, 1080, 0, 0);
  EXPECT_TRUE(matches_mode(d, 1920, 1080, 60));
  d.refresh = {60000, 1001};
  EXPECT_TRUE(matches_mode(d, 1920, 1080, 60));  // Moonlight sends 59.94 Hz as 60
  EXPECT_FALSE(matches_mode(d, 1920, 1080, 120));
  EXPECT_FALSE(matches_mode(d, 2560, 1440, 60));
}

TEST(Multistream, PlansStreamsWithinTheLimits) {
  EXPECT_EQ(plan_streams(1, {}).streams, 1u);
  EXPECT_EQ(plan_streams(3, {}).streams, 3u);
  EXPECT_EQ(plan_streams(3, {}).reason, TESSERA_REASON_NONE);

  const auto host_limited = plan_streams(6, {});
  EXPECT_EQ(host_limited.streams, static_cast<std::uint32_t>(TESSERA_MAX_STREAMS));
  EXPECT_EQ(host_limited.reason, TESSERA_REASON_HOST_LIMIT);

  const auto encoder_limited = plan_streams(4, {.encoder_max = 2});
  EXPECT_EQ(encoder_limited.streams, 2u);
  EXPECT_EQ(encoder_limited.reason, TESSERA_REASON_ENCODER_LIMIT);

  EXPECT_EQ(plan_streams(4, {.host_max = 3, .encoder_max = 3}).reason, TESSERA_REASON_HOST_LIMIT);
  EXPECT_EQ(plan_streams(4, {.permitted = false}).reason, TESSERA_REASON_NOT_PERMITTED);
  EXPECT_EQ(plan_streams(4, {.permitted = false}).streams, 1u);
  EXPECT_EQ(plan_streams(4, {.driver_available = false}).reason, TESSERA_REASON_NO_DISPLAY_DRIVER);
  EXPECT_EQ(plan_streams(4, {.encoder_max = 0}).streams, 1u);

  EXPECT_EQ(reason_token(TESSERA_REASON_ENCODER_LIMIT), "encoder-limit");
  EXPECT_EQ(reason_token(static_cast<tessera_reason_e>(15)), "unknown");
}

TEST(Multistream, KeepsAConnectedClientLayout) {
  // An L-shape around a negative origin: left monitor, primary, one above the primary.
  const std::vector<client_display_t> displays {
    display(1, 2560, 1440, 0, 0),
    display(2, 1920, 1080, -1920, 360),
    display(3, 1920, 1080, 320, -1080),
  };
  const auto rects = layout(displays);
  ASSERT_EQ(rects.size(), 3u);
  EXPECT_EQ(rects[0], (rect_t {1920, 1080, 2560, 1440}));
  EXPECT_EQ(rects[1], (rect_t {0, 1440, 1920, 1080}));
  EXPECT_EQ(rects[2], (rect_t {2240, 0, 1920, 1080}));
}

TEST(Multistream, FallsBackToARowWhenTheLayoutIsUnusable) {
  // Overlapping (e.g. a client that reported positions in different units).
  const auto overlapping = layout({display(1, 1920, 1080, 0, 0), display(2, 1920, 1080, 1000, 0)});
  EXPECT_EQ(overlapping[0], (rect_t {0, 0, 1920, 1080}));
  EXPECT_EQ(overlapping[1], (rect_t {1920, 0, 1920, 1080}));

  // A gap, and touching only at a corner: both disconnected. Ordered by x, top edges aligned.
  const auto gap = layout({display(1, 1920, 1080, 3000, 0), display(2, 1280, 720, 0, 500)});
  EXPECT_EQ(gap[1], (rect_t {0, 0, 1280, 720}));
  EXPECT_EQ(gap[0], (rect_t {1280, 0, 1920, 1080}));

  const auto corner = layout({display(1, 1920, 1080, 0, 0), display(2, 1920, 1080, 1920, 1080)});
  EXPECT_EQ(corner[1], (rect_t {1920, 0, 1920, 1080}));
}

TEST(Multistream, MonitorIdsAreStablePerClientAndMonitor) {
  const auto a = monitor_id("cert-A", {1, 2});
  const auto a_again = monitor_id("cert-A", {1, 2});
  const auto other_monitor = monitor_id("cert-A", {1, 3});
  const auto other_client = monitor_id("cert-B", {1, 2});
  ASSERT_TRUE(a && a_again && other_monitor && other_client);
  EXPECT_EQ(a.value(), a_again.value());
  EXPECT_NE(a.value(), other_monitor.value());
  EXPECT_NE(a.value(), other_client.value());
}

TEST(Multistream, DerivesThePhysicalSizeFromTheDpi) {
  EXPECT_EQ(physical_size_mm(1920, 1080, 0), (std::pair<std::uint32_t, std::uint32_t> {0, 0}));
  EXPECT_EQ(physical_size_mm(1920, 1080, 96), (std::pair<std::uint32_t, std::uint32_t> {508, 286}));  // 20" x 11.25"
  EXPECT_EQ(physical_size_mm(2560, 1440, 109), (std::pair<std::uint32_t, std::uint32_t> {597, 336}));  // a 27" 1440p
}

TEST(Multistream, CapsTheTotalBitrateProportionally) {
  EXPECT_EQ(cap_bitrates({32000, 18000}, 0), (std::vector<std::uint32_t> {32000, 18000}));
  EXPECT_EQ(cap_bitrates({32000, 18000}, 60000), (std::vector<std::uint32_t> {32000, 18000}));
  EXPECT_EQ(cap_bitrates({32000, 18000}, 25000), (std::vector<std::uint32_t> {16000, 9000}));
  EXPECT_EQ(cap_bitrates({1, 100000}, 1000), (std::vector<std::uint32_t> {1, 999}));  // never 0
}
