/**
 * @file src/tessera/multistream_host.h
 * @brief [fork] Tessera: the host side of multistream (protocol/multistream.md), called from nvhttp, rtsp and stream.
 */
#pragma once

// standard includes
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// lib includes
#include <boost/property_tree/ptree.hpp>

// local includes
#include "tessera/multistream.h"
#include "tessera/protocol.h"

namespace rtsp_stream {
  struct launch_session_t;
}

namespace stream {
  struct config_t;
}

namespace tessera::host {

  /**
   * @brief One stream of a multistream session.
   */
  struct stream_plan_t {
    multistream::client_display_t client;  ///< The client monitor it shows.
    std::string display;  ///< The host display it captures (\\.\DISPLAYn).
    std::string ping_payload;  ///< Streams 1 .. n-1: the ping payload their RTSP SETUP returns.
  };

  /**
   * @brief What the host decided for a launch request with a display list.
   */
  struct launch_plan_t {
    std::size_t requested = 0;  ///< Displays the client asked for.
    std::vector<stream_plan_t> streams;  ///< Two or more for a multistream session, otherwise empty.
    tessera_reason_e reason = TESSERA_REASON_NONE;  ///< Why the session got fewer streams than requested.
  };

  /// Adds the multistream capability to the /serverinfo reply.
  void add_serverinfo(boost::property_tree::ptree &tree);

  /**
   * @brief Reads the display list of a /launch or /resume request (`tmsv`, `tmsd`), decides how many streams the
   *        session gets and which display each captures. Requests without a display list stay ordinary sessions.
   */
  void plan_launch(rtsp_stream::launch_session_t &launch_session, std::string_view version, std::string_view displays);

  /// Removes the app's virtual displays and restores the host's topology (where Sunshine reverts its display setup).
  void revert_displays();

  /// Adds TesseraStreams / TesseraReason to the launch or resume reply, if the request had a display list.
  void add_launch_reply(const rtsp_stream::launch_session_t &launch_session, boost::property_tree::ptree &tree);

  /// Streams in the session: 1 for an ordinary session.
  std::size_t stream_count(const rtsp_stream::launch_session_t &launch_session);

  /// true for a multistream session.
  inline bool is_multistream(const rtsp_stream::launch_session_t &launch_session) {
    return stream_count(launch_session) > 1;
  }

  /**
   * @brief The ping payload for an RTSP SETUP target.
   * @return The payload of extra stream i for "streamid=video/<i>/0"; nullopt for stream 0 and other targets; an
   *         empty string if i names no stream of this session.
   */
  std::optional<std::string> setup_ping_payload(const rtsp_stream::launch_session_t &launch_session, std::string_view target);

  /// The ping payload of extra stream `stream` (1 .. n-1).
  std::string ping_payload(const rtsp_stream::launch_session_t &launch_session, int stream);

  /**
   * @brief ANNOUNCE: sets each stream's display and builds the extra streams' encoder settings from their
   *        `x-nv-video[i]` / `x-nv-vqos[i]` attributes.
   * @return false if the request is invalid (the caller answers 400).
   */
  bool apply_announce(const rtsp_stream::launch_session_t &launch_session, const std::unordered_map<std::string_view, std::string_view> &args, stream::config_t &config);

}  // namespace tessera::host
