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
#include <string>

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

    /// Restarts the driver with `count` displays (0 disables the device).
    virtual status_t set_count(std::uint32_t count, const display_mode_t &mode);

    /// Keeps the added displays alive; called from a separate thread every keepalive_interval().
    virtual status_t keepalive() {
      return status_t::ok();
    }
  };

  struct backend_options_t {
    std::string mtt_config = "C:\\VirtualDisplayDriver\\vdd_settings.xml";  ///< MTT VDD settings file.
  };

  /// "sudovda", "parsec" or "mtt".
  result_t<std::unique_ptr<backend_t>> make_backend(const std::string &name, const backend_options_t &options);

}  // namespace tessera::stress
