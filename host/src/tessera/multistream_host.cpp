/**
 * @file src/tessera/multistream_host.cpp
 * @brief [fork] Tessera: the host side of multistream (protocol/multistream.md).
 */
// standard includes
#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <tuple>

// local includes
#include "multistream_host.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/rtsp.h"
#include "src/stream.h"
#include "src/video.h"
#include "tessera/crypto.h"
#include "tessera/display_topology.h"

using namespace std::literals;

namespace tessera::host {

  namespace {
    std::optional<std::int64_t> number(std::string_view text) {
      std::int64_t value = 0;
      const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
      if (error != std::errc {} || end != text.data() + text.size()) {
        return std::nullopt;
      }
      return value;
    }

    /// Streams the host can run now, stream 0 included.
    std::uint32_t host_max_streams() {
      return video::tessera_supports_multistream() ? TESSERA_MAX_STREAMS : 1;
    }

    /**
     * @brief The active displays to stream, the primary first and the rest left to right.
     * @details Phase 2 step 3 streams existing displays; virtual displays per client monitor come with the display
     *          session.
     */
    std::vector<std::string> active_displays() {
      const auto topology = display::capture();
      if (!topology) {
        BOOST_LOG(error) << "Multistream: can't read the display topology: "sv << topology.error().to_string();
        return {};
      }
      std::vector<display::path_t> sources;
      for (const auto &path : topology->paths) {
        // Clones share a source; stream each source once
        const bool seen = std::ranges::any_of(sources, [&path](const display::path_t &s) {
          return s.source_adapter_path == path.source_adapter_path && s.source_id == path.source_id;
        });
        if (!seen) {
          sources.push_back(path);
        }
      }
      std::ranges::sort(sources, [](const display::path_t &a, const display::path_t &b) {
        if (a.is_primary() != b.is_primary()) {
          return a.is_primary();
        }
        return std::tie(a.position_x, a.position_y) < std::tie(b.position_x, b.position_y);
      });
      std::vector<std::string> names;
      for (const auto &source : sources) {
        names.push_back(source.gdi_name);
      }
      return names;
    }

    std::string random_ping_payload() {
      std::array<std::uint8_t, 8> raw {};
      if (!crypto::random_bytes(raw.data(), raw.size())) {
        return {};
      }
      return crypto::to_hex(raw.data(), raw.size());  // 16 characters, as Sunshine's own ping payload
    }

    const launch_plan_t *plan_of(const rtsp_stream::launch_session_t &launch_session) {
      return launch_session.tessera.get();
    }
  }  // namespace

  void add_serverinfo(boost::property_tree::ptree &tree) {
    tree.put("root."s + TESSERA_SERVERINFO_MULTISTREAM, TESSERA_MULTISTREAM_VERSION);
    tree.put("root."s + TESSERA_SERVERINFO_MAX_STREAMS, host_max_streams());
  }

  void plan_launch(rtsp_stream::launch_session_t &launch_session, std::string_view version, std::string_view displays) {
    launch_session.tessera.reset();
    if (displays.empty()) {
      return;
    }

    auto plan = std::make_shared<launch_plan_t>();
    launch_session.tessera = plan;
    const auto single = [&plan](tessera_reason_e reason) {
      plan->streams.clear();
      plan->reason = reason;
      BOOST_LOG(info) << "Multistream: one stream ("sv << multistream::reason_token(reason) << ')';
    };

    if (number(version).value_or(0) < 1) {
      single(TESSERA_REASON_INVALID_REQUEST);
      return;
    }
    auto requested = multistream::parse_display_list(displays);
    if (!requested) {
      BOOST_LOG(warning) << "Multistream: invalid display list: "sv << requested.error().to_string();
      single(TESSERA_REASON_INVALID_REQUEST);
      return;
    }
    plan->requested = requested->size();
    if (!multistream::matches_mode(requested->front(), launch_session.width, launch_session.height, launch_session.fps)) {
      BOOST_LOG(warning) << "Multistream: display 0 doesn't match the launch mode"sv;
      single(TESSERA_REASON_INVALID_REQUEST);
      return;
    }
    if (!video::tessera_supports_multistream()) {
      single(TESSERA_REASON_UNSUPPORTED);
      return;
    }

    multistream::limits_t limits;
    limits.host_max = host_max_streams();
    auto decision = multistream::plan_streams(requested->size(), limits);

    auto host_displays = active_displays();
    if (host_displays.size() < decision.streams) {
      decision.streams = static_cast<std::uint32_t>(std::max<std::size_t>(host_displays.size(), 1));
      decision.reason = TESSERA_REASON_HOST_LIMIT;
    }
    if (decision.streams < 2) {
      single(decision.reason);
      return;
    }

    for (std::uint32_t i = 0; i < decision.streams; ++i) {
      stream_plan_t stream;
      stream.client = (*requested)[i];
      stream.display = host_displays[i];
      if (i > 0) {
        stream.ping_payload = random_ping_payload();
        if (stream.ping_payload.empty()) {
          single(TESSERA_REASON_HOST_LIMIT);
          return;
        }
      }
      plan->streams.push_back(std::move(stream));
    }
    plan->reason = decision.reason;
    BOOST_LOG(info) << "Multistream: "sv << plan->streams.size() << " of "sv << plan->requested << " displays"sv
                    << (decision.reason == TESSERA_REASON_NONE ? ""s : " ("s + std::string(multistream::reason_token(decision.reason)) + ')');
    for (std::size_t i = 0; i < plan->streams.size(); ++i) {
      const auto &s = plan->streams[i];
      BOOST_LOG(info) << "Multistream: stream "sv << i << ": "sv << s.client.width << 'x' << s.client.height << " on "sv << s.display;
    }
  }

  void add_launch_reply(const rtsp_stream::launch_session_t &launch_session, boost::property_tree::ptree &tree) {
    const auto *plan = plan_of(launch_session);
    if (!plan) {
      return;
    }
    tree.put("root."s + TESSERA_LAUNCH_STREAMS, stream_count(launch_session));
    if (plan->reason != TESSERA_REASON_NONE) {
      tree.put("root."s + TESSERA_LAUNCH_REASON, std::string(multistream::reason_token(plan->reason)));
    }
  }

  std::size_t stream_count(const rtsp_stream::launch_session_t &launch_session) {
    const auto *plan = plan_of(launch_session);
    return plan && plan->streams.size() > 1 ? plan->streams.size() : 1;
  }

  std::optional<std::string> setup_ping_payload(const rtsp_stream::launch_session_t &launch_session, std::string_view target) {
    // "streamid=video/<i>/0"
    constexpr auto prefix = "streamid=video/"sv;
    const auto at = target.find(prefix);
    if (at == std::string_view::npos) {
      return std::nullopt;
    }
    auto rest = target.substr(at + prefix.size());
    const auto index = number(rest.substr(0, rest.find('/')));
    if (!index || *index == 0) {
      return std::nullopt;
    }
    if (*index < 0 || static_cast<std::size_t>(*index) >= stream_count(launch_session)) {
      return ""s;
    }
    return ping_payload(launch_session, static_cast<int>(*index));
  }

  std::string ping_payload(const rtsp_stream::launch_session_t &launch_session, int stream) {
    const auto *plan = plan_of(launch_session);
    if (!plan || stream < 1 || static_cast<std::size_t>(stream) >= plan->streams.size()) {
      return {};
    }
    return plan->streams[stream].ping_payload;
  }

  bool apply_announce(const rtsp_stream::launch_session_t &launch_session, const std::unordered_map<std::string_view, std::string_view> &args, stream::config_t &config) {
    config.tessera_monitors.clear();
    const auto *plan = plan_of(launch_session);
    if (!plan || plan->streams.size() < 2) {
      return true;
    }

    // Stream 0 shows the client's primary monitor on the planned display, whatever the client does next
    config.monitor.tessera_display = plan->streams[0].display;
    config.monitor.tessera_stream = 0;

    const auto get = [&args](const std::string &name) -> std::optional<std::int64_t> {
      const auto it = args.find(name);
      return it == args.end() ? std::nullopt : number(it->second);
    };

    // How many streams the client runs (it may have dropped some after a failed SETUP)
    const auto confirmed = std::clamp<std::int64_t>(get(TESSERA_SDP_STREAMS).value_or(1), 1, static_cast<std::int64_t>(plan->streams.size()));

    for (int i = 1; i < confirmed; ++i) {
      const auto video = std::format("x-nv-video[{}].", i);
      const auto vqos = std::format("x-nv-vqos[{}].", i);
      const auto width = get(video + "clientViewportWd");
      const auto height = get(video + "clientViewportHt");
      const auto fps = get(video + "maxFPS");
      const auto max_bitrate = get(vqos + "bw.maximumBitrateKbps");
      if (!width || !height || !fps || !max_bitrate || *width < 1 || *width > 8192 || *height < 1 || *height > 8192 ||
          *fps < 1 || *fps > 1000 || *max_bitrate < 1) {
        BOOST_LOG(error) << "Multistream: stream "sv << i << " has missing or invalid attributes"sv;
        return false;
      }

      // Codec, colour space, chroma and intra refresh as stream 0; the rest per stream
      video::config_t monitor = config.monitor;
      monitor.width = static_cast<int>(*width);
      monitor.height = static_cast<int>(*height);
      monitor.framerate = static_cast<int>(*fps);
      monitor.framerateX100 = static_cast<int>(get(video + "clientRefreshRateX100").value_or(*fps * 100));
      monitor.slicesPerFrame = static_cast<int>(std::clamp<std::int64_t>(get(video + "videoEncoderSlicesPerFrame").value_or(1), 1, 64));
      monitor.numRefFrames = static_cast<int>(std::clamp<std::int64_t>(get(video + "maxNumReferenceFrames").value_or(1), 0, 16));
      monitor.dynamicRange = get(video + "dynamicRangeMode").value_or(0) != 0 ? 1 : 0;

      // As for stream 0: with a configured bitrate, leave room for FEC; otherwise use the maximum the client sent
      if (auto configured = get(std::format("x-ml-video[{}].configuredBitrateKbps", i)); configured && *configured > 0) {
        auto kbps = static_cast<double>(*configured);
        if (config::stream.fec_percentage <= 80) {
          kbps /= 100.0 / (100 - config::stream.fec_percentage);
        }
        monitor.bitrate = std::max(1, static_cast<int>(kbps));
      } else {
        monitor.bitrate = static_cast<int>(std::min<std::int64_t>(*max_bitrate, 1'000'000));
      }

      monitor.tessera_display = plan->streams[i].display;
      monitor.tessera_stream = i;
      config.tessera_monitors.push_back(std::move(monitor));
    }

    // The host's bitrate limit applies to all streams together
    if (config::video.max_bitrate > 0) {
      std::vector<std::uint32_t> kbps {static_cast<std::uint32_t>(config.monitor.bitrate)};
      for (const auto &m : config.tessera_monitors) {
        kbps.push_back(static_cast<std::uint32_t>(m.bitrate));
      }
      kbps = multistream::cap_bitrates(std::move(kbps), static_cast<std::uint32_t>(config::video.max_bitrate));
      config.monitor.bitrate = static_cast<int>(kbps[0]);
      for (std::size_t i = 0; i < config.tessera_monitors.size(); ++i) {
        config.tessera_monitors[i].bitrate = static_cast<int>(kbps[i + 1]);
      }
    }

    BOOST_LOG(info) << "Multistream: client runs "sv << confirmed << " streams"sv;
    return true;
  }

}  // namespace tessera::host
