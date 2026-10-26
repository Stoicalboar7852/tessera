/**
 * @file host-helpers/common/include/tessera/vdd_client.h
 * @brief Client for the Tessera virtual display driver's control interface (drivers/vdd/include/tessera_vdd_ioctl.h).
 *
 * Monitors belong to the handle that added them: when a vdd_client_t is destroyed (or the process exits), the
 * driver removes its monitors after a 2 s grace period unless another handle adds the same IDs again first.
 * Opening the interface needs SYSTEM or Administrators.
 */
#pragma once

// standard includes
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// platform includes
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// local includes
#include "tessera/result.h"
#include "tessera/win32.h"
#include "tessera_vdd_ioctl.h"

namespace tessera::vdd {

  using monitor_id_t = std::array<std::uint8_t, 16>;

  /// A new random monitor ID (for tests and ad-hoc monitors; the host derives stable IDs per client monitor).
  result_t<monitor_id_t> random_monitor_id();

  struct monitor_spec_t {
    monitor_id_t id {};
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;
    std::uint32_t refresh_numerator = 60;  ///< 60000/1001 = 59.94 Hz
    std::uint32_t refresh_denominator = 1;
    bool hdr = false;
    std::uint32_t width_mm = 0;  ///< 0 = 96 DPI from the preferred mode
    std::uint32_t height_mm = 0;
    std::wstring name;  ///< Up to 31 characters; the EDID shows the first 13 ASCII characters
  };

  struct added_monitor_t {
    monitor_id_t id {};
    std::uint32_t target_id = 0;  ///< OS target ID (as in DISPLAYCONFIG paths)
    LUID adapter {};  ///< OS adapter LUID
    bool reclaimed = false;  ///< The monitor already existed (e.g. orphaned by a previous host) and was taken over
  };

  /// Builds an error from an NTSTATUS returned by the driver.
  error_t ntstatus_error(std::int32_t status, std::string where, std::string_view context = {});

  class vdd_client_t {
  public:
    /// Opens the first Tessera VDD control interface.
    static result_t<vdd_client_t> open();

    result_t<tessera_vdd_version_t> version() const;

    /// Asks the driver to render on this GPU (normally the one that encodes).
    status_t set_render_adapter(const LUID &adapter) const;

    /**
     * @brief Adds up to TESSERA_VDD_MAX_MONITORS monitors in one request; they arrive back to back.
     * @return One result per spec (in order), or an error if the request as a whole failed.
     */
    result_t<std::vector<result_t<added_monitor_t>>> add(const std::vector<monitor_spec_t> &monitors) const;

    /// Removes monitors added through this client. One status per ID.
    result_t<std::vector<status_t>> remove(const std::vector<monitor_id_t> &ids) const;

    /// Every monitor of the adapter, with ownership and swap-chain state.
    result_t<std::vector<tessera_vdd_list_entry_t>> list() const;

  private:
    explicit vdd_client_t(win32::unique_handle_t handle):
        handle_(std::move(handle)) {}

    win32::unique_handle_t handle_;
  };

}  // namespace tessera::vdd
