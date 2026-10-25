/**
 * @file host-helpers/tests/display_topology_test.cpp
 * @brief Unit tests for topology serialisation and comparison (no display changes are made).
 */
// lib includes
#include <gtest/gtest.h>

// local includes
#include "tessera/display_topology.h"

using nlohmann::json;
using namespace tessera::display;

namespace {

  path_t make_path(std::uint32_t target, std::int32_t x, std::uint32_t w = 1920, std::uint32_t h = 1080) {
    path_t p;
    p.adapter_path = R"(\\?\PCI#VEN_10DE&DEV_2805#4&d0bdf66&0&0009#{5b45201d-f2f2-4f3b-85bb-30ff1f953599})";
    p.source_adapter_path = p.adapter_path;
    p.source_id = target;
    p.target_id = 100 + target;
    p.monitor_path = "\\\\?\\DISPLAY#DELA0DC#" + std::to_string(target);
    p.monitor_name = "Monitor " + std::to_string(target);
    p.gdi_name = "\\\\.\\DISPLAY" + std::to_string(target + 1);
    p.width = w;
    p.height = h;
    p.position_x = x;
    p.position_y = 0;
    p.pixel_format = 4;  // 32bpp
    p.refresh_rate = {60000, 1001};
    p.rotation = 1;
    p.scaling = 1;
    p.scanline_ordering = 1;
    p.hdr_supported = true;
    p.hdr_enabled = false;
    target_timing_t t;
    t.pixel_rate = 148500000;
    t.h_sync = {67500, 1};
    t.v_sync = {60000, 1001};
    t.active_width = w;
    t.active_height = h;
    t.total_width = 2200;
    t.total_height = 1125;
    t.video_standard = 255;
    t.scanline_ordering = 1;
    p.timing = t;
    return p;
  }

  topology_t two_monitors() {
    topology_t t;
    t.paths.push_back(make_path(0, 0));
    t.paths.push_back(make_path(1, 1920));
    return t;
  }

}  // namespace

TEST(DisplayTopology, JsonRoundTrip) {
  const auto t = two_monitors();
  auto back = from_json(to_json(t));
  ASSERT_TRUE(back) << back.error().to_string();
  ASSERT_EQ(back->paths.size(), 2u);
  EXPECT_TRUE(compare(t, back.value()).empty());
  EXPECT_EQ(back->paths[1].timing, t.paths[1].timing);
  EXPECT_EQ(back->paths[0].monitor_path, t.paths[0].monitor_path);
  EXPECT_EQ(back->paths[0].refresh_rate, (rational_t {60000, 1001}));
}

TEST(DisplayTopology, JsonRoundTripSurvivesTextSerialisation) {
  const auto t = two_monitors();
  auto back = from_json(json::parse(to_json(t).dump()));
  ASSERT_TRUE(back) << back.error().to_string();
  EXPECT_TRUE(compare(t, back.value()).empty());
}

TEST(DisplayTopology, FromJsonRejectsInvalidInput) {
  EXPECT_FALSE(from_json(json::object()));
  EXPECT_FALSE(from_json(json {{"paths", json::array()}}));

  auto j = to_json(two_monitors());
  j["paths"][0]["width"] = 0;
  EXPECT_FALSE(from_json(j));

  j = to_json(two_monitors());
  j["paths"][0]["position"] = json::array({1, 2, 3});
  EXPECT_FALSE(from_json(j));

  j = to_json(two_monitors());
  j["paths"][0]["refresh_rate"] = "60";
  EXPECT_FALSE(from_json(j));

  j = to_json(two_monitors());
  j["paths"][0]["adapter_path"] = std::string(600, 'x');
  EXPECT_FALSE(from_json(j));

  j = to_json(two_monitors());
  j["paths"][0]["source_id"] = -1;
  EXPECT_FALSE(from_json(j));
}

TEST(DisplayTopology, FromJsonRequiresAPrimaryDisplay) {
  auto t = two_monitors();
  t.paths[0].position_x = 100;  // nothing at (0,0) any more
  EXPECT_FALSE(from_json(to_json(t)));
}

TEST(DisplayTopology, FromJsonRejectsTooManyPaths) {
  topology_t t;
  for (std::uint32_t i = 0; i < 65; ++i) {
    t.paths.push_back(make_path(i, static_cast<std::int32_t>(i) * 1920));
  }
  EXPECT_FALSE(from_json(to_json(t)));
}

TEST(DisplayTopology, CompareFindsEachKindOfDifference) {
  const auto expected = two_monitors();

  auto moved = expected;
  moved.paths[1].position_x = -1920;
  EXPECT_EQ(compare(expected, moved).size(), 1u);

  auto refresh = expected;
  refresh.paths[0].refresh_rate = {60, 1};  // 60 vs 59.94
  EXPECT_EQ(compare(expected, refresh).size(), 1u);

  auto rotated = expected;
  rotated.paths[0].rotation = 2;
  EXPECT_EQ(compare(expected, rotated).size(), 1u);

  auto hdr = expected;
  hdr.paths[1].hdr_enabled = true;
  EXPECT_EQ(compare(expected, hdr).size(), 1u);

  auto resized = expected;
  resized.paths[0].width = 2560;
  EXPECT_EQ(compare(expected, resized).size(), 1u);

  auto one_off = expected;
  one_off.paths.pop_back();
  EXPECT_EQ(compare(expected, one_off).size(), 1u);  // expected active, is inactive

  auto extra = expected;
  extra.paths.push_back(make_path(2, 3840));
  EXPECT_EQ(compare(expected, extra).size(), 1u);  // expected inactive, is active
}

TEST(DisplayTopology, CompareTreatsEquivalentRationalsAsEqual) {
  const auto expected = two_monitors();
  auto same = expected;
  same.paths[0].refresh_rate = {120000, 2002};
  EXPECT_TRUE(compare(expected, same).empty());
}

TEST(DisplayTopology, CompareIgnoresHdrWhenUnsupported) {
  auto expected = two_monitors();
  expected.paths[0].hdr_supported = false;
  auto actual = expected;
  actual.paths[0].hdr_enabled = true;
  actual.paths[0].hdr_supported = false;
  EXPECT_TRUE(compare(expected, actual).empty());
}

TEST(DisplayTopology, ApplyRefusesEmptyTopology) {
  auto st = apply(topology_t {});
  ASSERT_FALSE(st);
  EXPECT_EQ(st.error().code, static_cast<std::uint32_t>(tessera::tessera_code_e::unsafe));
}

TEST(DisplayTopology, CaptureOnThisMachineRoundTrips) {
  // Read-only: captures the real topology (if any display is active) and checks it serialises.
  auto t = capture();
  ASSERT_TRUE(t) << t.error().to_string();
  if (t->paths.empty()) {
    GTEST_SKIP() << "no active display (headless runner)";
  }
  auto back = from_json(json::parse(to_json(t.value()).dump()));
  ASSERT_TRUE(back) << back.error().to_string();
  EXPECT_TRUE(compare(t.value(), back.value()).empty());
}
