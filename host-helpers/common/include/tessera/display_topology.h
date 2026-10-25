/**
 * @file host-helpers/common/include/tessera/display_topology.h
 * @brief Snapshot and exact restore of the Windows display topology through the CCD API
 *        (QueryDisplayConfig / SetDisplayConfig), for the state journal (brief §4.2, F3).
 *
 * A snapshot records every *active* path: which source drives which target, desktop position,
 * resolution, refresh rate (exact rational), rotation, scaling, scanline ordering, the full target
 * timing, and the HDR (advanced colour) state. Primary = the source at desktop position (0, 0).
 *
 * Adapter LUIDs change across reboots and driver restarts, so paths are stored with stable identities
 * (adapter device path + source/target id, and the monitor device path) and mapped back to the current
 * LUIDs at restore time.
 *
 * Not captured: per-monitor DPI scaling (no public API; Windows stores it per monitor itself),
 * SDR white level (no public setter), and "virtual mode" desktop scaling (QDC_VIRTUAL_MODE_AWARE).
 */
#pragma once

// standard includes
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "tessera/result.h"

namespace tessera::display {

  struct rational_t {
    std::uint32_t numerator = 0;
    std::uint32_t denominator = 0;

    bool operator==(const rational_t &) const = default;
  };

  /// Full target timing (DISPLAYCONFIG_VIDEO_SIGNAL_INFO).
  struct target_timing_t {
    std::uint64_t pixel_rate = 0;
    rational_t h_sync;
    rational_t v_sync;
    std::uint32_t active_width = 0;
    std::uint32_t active_height = 0;
    std::uint32_t total_width = 0;
    std::uint32_t total_height = 0;
    std::uint32_t video_standard = 0;  ///< Raw `videoStandard` bitfield word.
    std::uint32_t scanline_ordering = 0;

    bool operator==(const target_timing_t &) const = default;
  };

  /// One active display path.
  struct path_t {
    // Stable identity
    std::string adapter_path;  ///< DISPLAYCONFIG_ADAPTER_NAME of the target's adapter.
    std::string source_adapter_path;  ///< Adapter of the source; usually equal to adapter_path.
    std::uint32_t source_id = 0;
    std::uint32_t target_id = 0;
    std::string monitor_path;  ///< DISPLAYCONFIG_TARGET_DEVICE_NAME::monitorDevicePath (may be empty).
    std::string monitor_name;  ///< Friendly name, for logs and UI only.
    std::string gdi_name;  ///< \\.\DISPLAYn, for logs only (not stable).

    // Source mode (desktop)
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int32_t position_x = 0;
    std::int32_t position_y = 0;
    std::uint32_t pixel_format = 0;  ///< DISPLAYCONFIG_PIXELFORMAT

    // Target
    rational_t refresh_rate;
    std::uint32_t rotation = 0;  ///< DISPLAYCONFIG_ROTATION
    std::uint32_t scaling = 0;  ///< DISPLAYCONFIG_SCALING
    std::uint32_t scanline_ordering = 0;
    std::uint32_t output_technology = 0;  ///< DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY (informational)
    std::optional<target_timing_t> timing;  ///< Absent if the driver reported no target mode.

    // HDR
    bool hdr_supported = false;
    bool hdr_enabled = false;

    bool is_primary() const {
      return position_x == 0 && position_y == 0;
    }
  };

  struct topology_t {
    std::vector<path_t> paths;  ///< Active paths only. Clones share adapter_path + source_id.
  };

  /// Captures the current active topology.
  result_t<topology_t> capture();

  /// Number of active paths right now (0 means no visible output).
  result_t<std::size_t> active_path_count();

  struct restore_options_t {
    bool restore_hdr = true;
  };

  /// Field-by-field differences between two topologies; empty means equal for restore purposes.
  std::vector<std::string> compare(const topology_t &expected, const topology_t &actual);

  /**
   * @brief Applies `wanted` as the active topology and verifies it.
   *
   * Paths in `wanted` whose adapter or target is not present any more are reported as an error
   * (nothing is applied). Restore tries the exact saved timing first, then lets Windows pick a timing
   * for the saved resolution and refresh rate. Returns an error with the list of remaining differences
   * if verification fails.
   */
  status_t apply(const topology_t &wanted, const restore_options_t &options = {});

  /// Last-resort recovery: asks Windows to re-apply the configuration it has stored for the currently
  /// connected monitors (SDC_USE_DATABASE_CURRENT). Used when an exact restore is impossible.
  status_t apply_database_current();

  /// JSON (de)serialisation for the journal. from_json validates sizes and required fields.
  nlohmann::json to_json(const topology_t &t);
  result_t<topology_t> from_json(const nlohmann::json &j);

  /// One line per path, for logs.
  std::string describe(const topology_t &t);

}  // namespace tessera::display
