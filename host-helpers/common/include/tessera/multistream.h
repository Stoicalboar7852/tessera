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

  /// What a launch with a display list does with the virtual displays an earlier launch created.
  enum class display_reuse_e {
    create,  ///< There are none: create this client's.
    reuse,  ///< They show the same client monitors in the same modes: keep them.
    share,  ///< Another client is streaming them: show them to this client too, unchanged (one set at a time).
    replace,  ///< Nobody is streaming them: remove them and create this client's.
  };

  /**
   * @brief Chooses what happens to the existing virtual displays.
   * @param displays_exist An earlier launch's virtual displays are still there.
   * @param same_monitors They were made for the same client monitors and modes as this launch asks for.
   * @param others_streaming A streaming session is running.
   */
  display_reuse_e choose_display_reuse(bool displays_exist, bool same_monitors, bool others_streaming);

  /// Limits a plan to the displays another client streams (`shared_displays` of them), with TESSERA_REASON_SHARED.
  plan_t share_displays(plan_t plan, std::size_t shared_displays);

  /// What happens to the virtual displays when the last streaming session ends while the app keeps running.
  enum class display_release_e {
    keep,  ///< There are none.
    remove_now,  ///< The user ended the stream (TESSERA_DISCONNECT_QUIT).
    remove_later,  ///< The connection was lost: keep them for a while, so the client can resume on them.
  };

  /**
   * @brief Chooses what happens to the virtual displays.
   * @param displays_exist Virtual displays are there.
   * @param client_quit The client's disconnect carried TESSERA_DISCONNECT_QUIT.
   */
  display_release_e choose_display_release(bool displays_exist, bool client_quit);

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
