/**
 * @file tools/mstream-client/src/session.cpp
 * @brief One streaming session on common-c with Tessera's multistream API.
 */
// platform includes
#include <windows.h>

// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

// lib includes
#include <openssl/rand.h>

extern "C" {
#include "Limelight.h"
#include "Tessera.h"
}

// local includes
#include "session.h"

namespace mstream {

  namespace {
    constexpr int https_port = 47984;

    /// Everything a stream's callbacks record. common-c's decoder callbacks carry no context, so this is global.
    struct stream_stats_t {
      std::mutex mutex;
      int width = 0;
      int height = 0;
      int video_format = 0;
      std::uint64_t frames = 0;
      std::uint64_t idr_frames = 0;
      std::uint64_t bytes = 0;
      std::chrono::steady_clock::time_point first_frame;
      std::vector<double> host_latency_ms;  ///< Host processing latency reported per frame.
      std::vector<double> assemble_ms;  ///< First packet received -> frame complete.
      std::ofstream dump;
      bool dumping = false;
      int status_state = -1;
      int status_reason = 0;
    };

    struct session_state_t {
      std::chrono::steady_clock::time_point start;
      int dump_seconds = 0;
      std::filesystem::path out_dir;
      std::array<stream_stats_t, TESSERA_MAX_STREAMS> streams;
      std::atomic<int> terminated {0};
      std::atomic<bool> has_terminated {false};
      std::FILE *log = nullptr;
    };

    session_state_t *state = nullptr;

    void log_line(const char *format, ...) {
      va_list args;
      va_start(args, format);
      if (state && state->log) {
        va_list copy;
        va_copy(copy, args);
        std::vfprintf(state->log, format, copy);
        va_end(copy);
        std::fflush(state->log);
      }
      va_end(args);
    }

    template<int N>
    int setup_stream(int video_format, int width, int height, int, void *, int) {
      auto &s = state->streams[N];
      std::lock_guard lock {s.mutex};
      s.width = width;
      s.height = height;
      s.video_format = video_format;
      const char *extension = (video_format & VIDEO_FORMAT_MASK_H265) ? ".h265" : (video_format & VIDEO_FORMAT_MASK_H264) ? ".h264" : ".av1";
      s.dump.open(state->out_dir / ("stream" + std::to_string(N) + extension), std::ios::binary | std::ios::trunc);
      s.dumping = s.dump.is_open() && state->dump_seconds > 0;
      return 0;
    }

    template<int N>
    int submit_stream(PDECODE_UNIT unit) {
      auto &s = state->streams[N];
      const auto now = std::chrono::steady_clock::now();
      std::lock_guard lock {s.mutex};
      if (s.frames == 0) {
        s.first_frame = now;
      }
      ++s.frames;
      s.bytes += static_cast<std::uint64_t>(unit->fullLength);
      if (unit->frameType == FRAME_TYPE_IDR) {
        ++s.idr_frames;
      }
      if (unit->frameHostProcessingLatency != 0) {
        s.host_latency_ms.push_back(unit->frameHostProcessingLatency / 10.0);
      }
      if (unit->enqueueTimeUs >= unit->receiveTimeUs) {
        s.assemble_ms.push_back(static_cast<double>(unit->enqueueTimeUs - unit->receiveTimeUs) / 1000.0);
      }
      if (s.dumping) {
        // Start at the first IDR frame, stop after dump_seconds
        if (s.dump.tellp() == 0 && unit->frameType != FRAME_TYPE_IDR) {
          return DR_NEED_IDR;
        }
        for (auto *entry = unit->bufferList; entry != nullptr; entry = entry->next) {
          s.dump.write(entry->data, entry->length);
        }
        if (now - s.first_frame > std::chrono::seconds(state->dump_seconds)) {
          s.dump.close();
          s.dumping = false;
        }
      }
      return DR_OK;
    }

    template<int N>
    void cleanup_stream() {
      auto &s = state->streams[N];
      std::lock_guard lock {s.mutex};
      if (s.dump.is_open()) {
        s.dump.close();
      }
    }

    template<int N>
    DECODER_RENDERER_CALLBACKS decoder_callbacks() {
      DECODER_RENDERER_CALLBACKS callbacks;
      LiInitializeVideoCallbacks(&callbacks);
      callbacks.setup = setup_stream<N>;
      callbacks.submitDecodeUnit = submit_stream<N>;
      callbacks.cleanup = cleanup_stream<N>;
      return callbacks;
    }

    void stream_status(int stream, int stream_state, int reason) {
      log_line("stream %d status %d reason %d\n", stream, stream_state, reason);
      if (stream >= 0 && stream < TESSERA_MAX_STREAMS) {
        std::lock_guard lock {state->streams[stream].mutex};
        state->streams[stream].status_state = stream_state;
        state->streams[stream].status_reason = reason;
      }
    }

    void connection_terminated(int error) {
      log_line("connection terminated: %d\n", error);
      state->terminated = error;
      state->has_terminated = true;
    }

    void log_message(const char *format, ...) {
      va_list args;
      va_start(args, format);
      if (state && state->log) {
        std::vfprintf(state->log, format, args);
        std::fflush(state->log);
      }
      va_end(args);
    }

    double percentile(std::vector<double> values, double p) {
      if (values.empty()) {
        return 0;
      }
      std::ranges::sort(values);
      const auto index = static_cast<std::size_t>(p / 100.0 * static_cast<double>(values.size() - 1) + 0.5);
      return values[std::min(index, values.size() - 1)];
    }

    std::string random_hex(std::size_t bytes) {
      std::string raw(bytes, '\0');
      RAND_bytes(reinterpret_cast<unsigned char *>(raw.data()), static_cast<int>(bytes));
      return to_hex(raw);
    }

    /// The app's ID from /applist.
    std::optional<std::string> app_id(const std::string &applist, const std::string &title) {
      const auto at = applist.find("<AppTitle>" + title + "</AppTitle>");
      if (at == std::string::npos) {
        return std::nullopt;
      }
      const auto app_end = applist.find("</App>", at);
      return xml_value(std::string_view(applist).substr(at, app_end - at), "ID");
    }
  }  // namespace

  nlohmann::json run_session(const session_options_t &options, const identity_t &identity) {
    nlohmann::json report;
    session_state_t session;
    session.start = std::chrono::steady_clock::now();
    session.dump_seconds = options.dump_seconds;
    session.out_dir = options.out_dir;
    std::filesystem::create_directories(session.out_dir);
    session.log = std::fopen((session.out_dir / "common-c.log").string().c_str(), "w");
    state = &session;
    const auto close_log = [&session] {
      if (session.log) {
        std::fclose(session.log);
        session.log = nullptr;
      }
      state = nullptr;
    };

    const auto fail = [&](const std::string &what) {
      report["error"] = what;
      close_log();
      return report;
    };

    const auto query_base = "?uniqueid=" + identity.unique_id + "&uuid=" + random_hex(16);
    auto serverinfo = http_get(options.host, https_port, true, "/serverinfo" + query_base, identity, 10000);
    if (!serverinfo) {
      return fail("serverinfo failed (not paired?)");
    }
    const auto app_version = xml_value(*serverinfo, "appversion").value_or("");
    const auto gfe_version = xml_value(*serverinfo, "GfeVersion").value_or("");
    const auto codec_support = std::atoi(xml_value(*serverinfo, "ServerCodecModeSupport").value_or("0").c_str());
    report["host"]["appversion"] = app_version;
    report["host"]["multistream"] = xml_value(*serverinfo, TESSERA_SERVERINFO_MULTISTREAM).value_or("");
    report["host"]["max_streams"] = xml_value(*serverinfo, TESSERA_SERVERINFO_MAX_STREAMS).value_or("");

    auto applist = http_get(options.host, https_port, true, "/applist" + query_base, identity, 10000);
    const auto app = applist ? app_id(*applist, options.app) : std::nullopt;
    if (!app) {
      return fail("app '" + options.app + "' not found");
    }

    // Displays left to right, display 0 the primary
    std::vector<TESSERA_DISPLAY> displays;
    int x = 0;
    for (std::size_t i = 0; i < options.displays.size(); ++i) {
      const auto &d = options.displays[i];
      TESSERA_DISPLAY td {};
      td.key[0] = 'T';
      td.key[1] = 'S';
      td.key[2] = static_cast<std::uint8_t>(i);
      td.keyLength = 3;
      td.width = d.width;
      td.height = d.height;
      td.refreshNumerator = d.refresh_numerator;
      td.refreshDenominator = d.refresh_denominator;
      td.x = x;
      td.y = 0;
      td.dpi = 96;
      td.hdr = d.hdr;
      x += d.width;
      displays.push_back(td);
    }
    const auto &primary = options.displays.front();
    const int fps = (primary.refresh_numerator + primary.refresh_denominator / 2) / primary.refresh_denominator;

    // Remote input key, as Moonlight does: rikey = the AES key, rikeyid = the first 4 IV bytes (big-endian)
    STREAM_CONFIGURATION config;
    LiInitializeStreamConfiguration(&config);
    RAND_bytes(reinterpret_cast<unsigned char *>(config.remoteInputAesKey), sizeof(config.remoteInputAesKey));
    std::uint32_t rikeyid = 0;
    RAND_bytes(reinterpret_cast<unsigned char *>(&rikeyid), sizeof(rikeyid));
    rikeyid &= 0x7FFFFFFF;
    config.remoteInputAesIv[0] = static_cast<char>(rikeyid >> 24);
    config.remoteInputAesIv[1] = static_cast<char>(rikeyid >> 16);
    config.remoteInputAesIv[2] = static_cast<char>(rikeyid >> 8);
    config.remoteInputAesIv[3] = static_cast<char>(rikeyid);
    config.width = primary.width;
    config.height = primary.height;
    config.fps = fps;
    config.bitrate = options.bitrate_kbps;
    config.packetSize = 1392;
    config.streamingRemotely = STREAM_CFG_LOCAL;
    config.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    config.supportedVideoFormats = options.hevc ? (VIDEO_FORMAT_H264 | VIDEO_FORMAT_H265) : VIDEO_FORMAT_H264;
    config.clientRefreshRateX100 = static_cast<int>(static_cast<long long>(primary.refresh_numerator) * 100 / primary.refresh_denominator);
    config.encryptionFlags = ENCFLG_ALL;
    config.colorSpace = COLORSPACE_REC_709;
    config.colorRange = COLOR_RANGE_LIMITED;

    std::string launch = "/launch" + query_base + "&appid=" + *app + "&mode=" + std::to_string(primary.width) + 'x' + std::to_string(primary.height) + 'x' + std::to_string(fps) +
                         "&additionalStates=1&sops=0&rikey=" + to_hex(std::string_view(config.remoteInputAesKey, 16)) + "&rikeyid=" + std::to_string(rikeyid) +
                         "&localAudioPlayMode=0&surroundAudioInfo=196610&remoteControllersBitmap=0&gcmap=0" + LiGetLaunchUrlQueryParameters();

    const TESSERA_CALLBACKS tessera_callbacks {stream_status, nullptr};
    const std::array<DECODER_RENDERER_CALLBACKS, TESSERA_MAX_STREAMS - 1> extra_callbacks {decoder_callbacks<1>(), decoder_callbacks<2>(), decoder_callbacks<3>()};
    if (!options.legacy && displays.size() > 1) {
      char list[TESSERA_DISPLAY_LIST_MAX_CHARS + 1];
      if (LiTesseraFormatDisplayList(displays.data(), static_cast<int>(displays.size()), list, sizeof(list)) < 0) {
        return fail("invalid display list");
      }
      launch += std::string("&") + TESSERA_LAUNCH_ARG_VERSION + "=1&" + TESSERA_LAUNCH_ARG_DISPLAYS + "=" + list;
      if (LiTesseraConfigure(displays.data(), static_cast<int>(std::min<std::size_t>(displays.size(), TESSERA_MAX_STREAMS)), extra_callbacks.data(), nullptr, &tessera_callbacks) != 0) {
        return fail("LiTesseraConfigure failed");
      }
    } else {
      LiTesseraReset();
    }

    auto launch_reply = http_get(options.host, https_port, true, launch, identity, 30000);
    if (!launch_reply || xml_status(*launch_reply) != 200) {
      return fail("launch failed: " + launch_reply.value_or("no reply"));
    }
    report["launch"]["streams"] = xml_value(*launch_reply, TESSERA_LAUNCH_STREAMS).value_or("");
    report["launch"]["reason"] = xml_value(*launch_reply, TESSERA_LAUNCH_REASON).value_or("");
    const auto session_url = xml_value(*launch_reply, "sessionUrl0").value_or("");

    SERVER_INFORMATION server;
    LiInitializeServerInformation(&server);
    server.address = options.host.c_str();
    server.serverInfoAppVersion = app_version.c_str();
    server.serverInfoGfeVersion = gfe_version.c_str();
    server.rtspSessionUrl = session_url.c_str();
    server.serverCodecModeSupport = codec_support;

    CONNECTION_LISTENER_CALLBACKS listener;
    LiInitializeConnectionCallbacks(&listener);
    listener.connectionTerminated = connection_terminated;
    listener.logMessage = log_message;
    auto stream0 = decoder_callbacks<0>();
    AUDIO_RENDERER_CALLBACKS audio;
    LiInitializeAudioCallbacks(&audio);

    const auto connect_start = std::chrono::steady_clock::now();
    const int err = LiStartConnection(&server, &config, &listener, &stream0, &audio, nullptr, 0, nullptr, 0);
    if (err != 0) {
      http_get(options.host, https_port, true, "/cancel" + query_base, identity, 10000);
      return fail("LiStartConnection failed: " + std::to_string(err));
    }
    const int streams = LiTesseraGetStreamCount();
    report["connect_ms"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - connect_start).count();
    report["streams"] = streams;

    // Stream; optionally move the mouse to each stream's centre and read the host cursor back (same PC)
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(options.seconds);
    if (options.mouse_test) {
      std::this_thread::sleep_for(std::chrono::seconds(5));
      for (int i = 0; i < streams; ++i) {
        const auto &d = options.displays[static_cast<std::size_t>(i)];
        LiTesseraSendMousePosition(i, static_cast<short>(d.width / 2), static_cast<short>(d.height / 2), static_cast<short>(d.width), static_cast<short>(d.height));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        POINT cursor {};
        GetCursorPos(&cursor);
        report["mouse"].push_back({{"stream", i}, {"cursor", {cursor.x, cursor.y}}});
      }
    }
    while (std::chrono::steady_clock::now() < deadline && !session.has_terminated) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::uint32_t rtt = 0;
    std::uint32_t rtt_variance = 0;
    if (LiGetEstimatedRttInfo(&rtt, &rtt_variance)) {
      report["rtt_ms"] = rtt;
    }
    for (int i = 0; i < streams; ++i) {
      if (const auto *rtp = LiTesseraGetRTPVideoStats(i)) {
        report["per_stream"][i]["rtp"] = {{"packets", rtp->packetCountVideo}, {"fec_packets", rtp->packetCountFec}, {"recovered", rtp->packetCountFecRecovered}, {"fec_failed", rtp->packetCountFecFailed}};
      }
    }

    LiStopConnection();
    http_get(options.host, https_port, true, "/cancel" + query_base, identity, 10000);
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - session.start).count();

    for (int i = 0; i < streams; ++i) {
      auto &s = session.streams[static_cast<std::size_t>(i)];
      std::lock_guard lock {s.mutex};
      auto &r = report["per_stream"][i];
      r["width"] = s.width;
      r["height"] = s.height;
      r["frames"] = s.frames;
      r["idr_frames"] = s.idr_frames;
      r["mbit_per_s"] = static_cast<double>(s.bytes) * 8 / 1e6 / std::max(1.0, elapsed);
      r["fps"] = s.frames > 1 ? static_cast<double>(s.frames) / std::max(1.0, std::chrono::duration<double>(std::chrono::steady_clock::now() - s.first_frame).count()) : 0.0;
      r["host_latency_ms"] = {{"p50", percentile(s.host_latency_ms, 50)}, {"p95", percentile(s.host_latency_ms, 95)}, {"max", percentile(s.host_latency_ms, 100)}};
      r["assemble_ms"] = {{"p50", percentile(s.assemble_ms, 50)}, {"p95", percentile(s.assemble_ms, 95)}, {"max", percentile(s.assemble_ms, 100)}};
      r["status"] = {{"state", s.status_state}, {"reason", s.status_reason}};
    }
    if (session.has_terminated) {
      report["terminated"] = session.terminated.load();
    }
    LiTesseraReset();
    close_log();
    return report;
  }

}  // namespace mstream
