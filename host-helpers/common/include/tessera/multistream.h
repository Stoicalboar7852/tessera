/**
 * @file host-helpers/common/include/tessera/multistream.h
 * @brief Host side of the multistream extension: the client's display list, the stream plan, display layout,
 *        monitor IDs and bitrate limits. Pure functions; see protocol/multistream.md.
 */
#pragma once

// standard includes
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// local includes
#include "tessera/display_modes.h"
#include "tessera/protocol.h"
#include "tessera/result.h"

namespace tessera::multistream {

  /// One entry of the client's display list (`tmsd`).
  struct client_display_t {
    std::vector<std::uint8_t> key;  ///< The client's stable monitor key.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    modes::rational_t refresh;
    std::int32_t x = 0;  ///< Position in the client's layout, in the same pixel units as width and height.
    std::int32_t y = 0;
    std::uint32_t dpi = 0;  ///< 0 = unknown
    bool hdr = false;
  };

  /// Parses and validates a display list. Fails on any malformed or out-of-range field.
  result_t<std::vector<client_display_t>> parse_display_list(std::string_view text);

  /// Formats a display list (the inverse of parse_display_list).
  std::string format_display_list(const std::vector<client_display_t> &displays);

  /// true if the display matches Sunshine's launch mode (`mode=WxHxFPS`; FPS is an integer).
  bool matches_mode(const client_display_t &display, int width, int height, int fps);

  /// What limits the number of streams.
  struct limits_t {
    std::uint32_t host_max = TESSERA_MAX_STREAMS;  ///< configuration
    std::uint32_t encoder_max = TESSERA_MAX_STREAMS;  ///< concurrent encoder sessions found at startup
    bool permitted = true;  ///< the client's "multi-display streaming" permission
    bool driver_available = true;  ///< the virtual display driver can be opened
  };

  struct plan_t {
    std::uint32_t streams = 1;  ///< 1 = an ordinary single-stream session
    tessera_reason_e reason = TESSERA_REASON_NONE;  ///< why streams < requested
  };

  /// How many of `requested` displays the session gets.
  plan_t plan_streams(std::size_t requested, const limits_t &limits);

  /// Reason token for the launch reply ("encoder-limit", ...).
  std::string_view reason_token(tessera_reason_e reason);

  struct rect_t {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    bool operator==(const rect_t &) const = default;
  };

  /**
   * @brief Where each display goes, relative to the top-left corner of the client's block (min x = min y = 0).
   *
   * Keeps the client's layout if its rectangles don't overlap and form one connected group (each touches another
   * along an edge). Otherwise places them left to right by x, top edges aligned.
   */
  std::vector<rect_t> layout(const std::vector<client_display_t> &displays);

  using monitor_id_t = std::array<std::uint8_t, 16>;

  /// Stable monitor ID: the first 16 bytes of SHA-256("tessera-monitor-v1" ‖ client certificate PEM ‖ key).
  result_t<monitor_id_t> monitor_id(std::string_view client_cert_pem, const std::vector<std::uint8_t> &key);

  /// Physical size in millimetres for the DPI; {0, 0} if the DPI is unknown (the driver then assumes 96 DPI).
  std::pair<std::uint32_t, std::uint32_t> physical_size_mm(std::uint32_t width, std::uint32_t height, std::uint32_t dpi);

  /// Scales every stream's bitrate alike so the sum is at most `max_total_kbps` (0 = no cap). Each stays ≥ 1.
  std::vector<std::uint32_t> cap_bitrates(std::vector<std::uint32_t> kbps, std::uint32_t max_total_kbps);

}  // namespace tessera::multistream
