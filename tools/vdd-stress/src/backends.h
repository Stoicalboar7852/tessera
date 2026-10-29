/**
 * @file tools/vdd-stress/src/backends.h
 * @brief Virtual display drivers that vdd-stress can add and remove displays with.
 *
 * Phase 1 uses the third-party drivers (SudoVDA, parsec-vdd, MTT VDD) to reproduce the multi-display freeze;
 * Tessera's own driver gets a backend here once it supports dynamic monitors, so the same stress run is its
 * acceptance test (brief §4.5, Phase 1).
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// local includes
#include "tessera/result.h"

namespace tessera::stress {

  struct display_mode_t {
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;
    std::uint32_t refresh_mhz = 60000;  ///< Refresh rate in millihertz (59940 = 59.94 Hz).

    std::string to_string() const;
  };

  /// Parses "1920x1080@60" or "1920x1080@59.94".
  result_t<display_mode_t> parse_mode(const std::string &text);

  /// A display target as the CCD API names it (adapter LUID + target ID).
  struct target_ref_t {
    std::uint32_t adapter_low = 0;
    std::int32_t adapter_high = 0;
    std::uint32_t id = 0;

    bool operator==(const target_ref_t &) const = default;
  };

  /**
   * @brief One way of creating virtual displays.
   */
  class backend_t {
  public:
    virtual ~backend_t() = default;

    virtual const char *name() const = 0;

    /// true: displays are added and removed one at a time (add/remove).
    /// false: the driver has a fixed display count that only changes when the device restarts (set_count).
    virtual bool dynamic() const = 0;

    /// How often keepalive() must be called while displays exist; zero if never.
    virtual std::chrono::milliseconds keepalive_interval() const = 0;

    /// Notes for the report (e.g. "mode is ignored: set by the driver's own configuration").
    virtual std::string notes() const {
      return {};
    }

    /// Opens the driver's control interface.
    virtual status_t open() = 0;

    /// Adds one display. Returns a handle for remove().
    virtual result_t<std::uint32_t> add(const display_mode_t &mode);

    virtual status_t remove(std::uint32_t handle);

    /// true if add_batch/remove_batch create or remove several displays in one driver request.
    virtual bool supports_batch() const {
      return false;
    }

    /// Adds `count` displays in one request; `handles` receives every display that was added, even on failure.
    virtual status_t add_batch(const display_mode_t &mode, std::uint32_t count, std::vector<std::uint32_t> &handles);

    /// Removes several displays in one request.
    virtual status_t remove_batch(const std::vector<std::uint32_t> &handles);

    /// The CCD target an added display became, if the driver reports it. Lets the runner wait for that display
    /// instead of counting active paths (Windows may switch other displays on or off meanwhile).
    virtual std::optional<target_ref_t> target_of(std::uint32_t) const {
      return std::nullopt;
    }

    /// Restarts the driver with `count` displays (0 disables the device).
    virtual status_t set_count(std::uint32_t count, const display_mode_t &mode);

    /// Keeps the added displays alive; called from a separate thread every keepalive_interval().
    virtual status_t keepalive() {
      return status_t::ok();
    }
  };

  struct backend_options_t {
    std::string mtt_config = "C:\\VirtualDisplayDriver\\vdd_settings.xml";  ///< MTT VDD settings file.
    bool hdr = false;  ///< tessera: add HDR10 monitors
    /// tessera: a new random monitor ID for every add, so Windows sees a never-seen monitor each time (worst case;
    /// each one leaves a monitor device and saved settings behind). Default: one stable ID per slot, like a host.
    bool new_ids = false;
  };

  /// "tessera", "sudovda", "parsec" or "mtt".
  result_t<std::unique_ptr<backend_t>> make_backend(const std::string &name, const backend_options_t &options);

}  // namespace tessera::stress
