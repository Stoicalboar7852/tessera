/**
 * @file src/tessera/display_session.h
 * @brief [fork] Tessera: the virtual displays of a multistream session (docs/phase-2-design.md, ADR 0006).
 */
#pragma once

// standard includes
#include <memory>
#include <string>
#include <vector>

// local includes
#include "tessera/multistream.h"
#include "tessera/result.h"

namespace tessera::host {

  /**
   * @brief One virtual display per client monitor, for as long as the app runs.
   * @details Creating it journals the host's display topology, adds every display in one request, and applies the
   *          client's layout to the right of the existing desktop (stream 0 as the primary, if configured) in one
   *          change. Reverting removes the displays in one request and puts the topology back. If the host dies, the
   *          driver removes the displays (they belong to its handle) and the service watchdog restores the topology
   *          from the journal.
   */
  class display_session_t {
  public:
    struct options_t {
      std::string client_name;  ///< For the display names (`<client>-<n>`).
      std::string client_cert;  ///< PEM; with each monitor key it makes the monitor's stable ID.
      bool make_primary = true;  ///< Make stream 0's display the Windows primary.
      std::string adapter_name;  ///< GPU to render on (Sunshine's adapter_name); empty = the high-performance GPU.
    };

    /**
     * @brief Creates the displays.
     * @return The session, whose `displays()` are the GDI names of the new displays in stream order.
     */
    static result_t<std::shared_ptr<display_session_t>> create(const std::vector<multistream::client_display_t> &clients, const options_t &options);

    ~display_session_t();

    display_session_t(const display_session_t &) = delete;
    display_session_t &operator=(const display_session_t &) = delete;

    /// \\.\DISPLAYn of each stream's display, in stream order.
    const std::vector<std::string> &displays() const {
      return displays_;
    }

    /// What the displays were made for (to reuse them on resume).
    const std::vector<multistream::client_display_t> &clients() const {
      return clients_;
    }

    /// Removes the displays and restores the topology (also done by the destructor).
    void revert();

  private:
    struct impl_t;

    display_session_t();

    std::unique_ptr<impl_t> impl_;
    std::vector<std::string> displays_;
    std::vector<multistream::client_display_t> clients_;
  };

}  // namespace tessera::host
