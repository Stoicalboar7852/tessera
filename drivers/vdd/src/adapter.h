/**
 * @file drivers/vdd/src/adapter.h
 * @brief The IddCx adapter of one Tessera VDD device: its monitor table, the control requests and the IddCx
 *        callbacks that need the table.
 *
 * Locking (docs/phase-1-design.md):
 *  - op_lock_ serialises the operations that create or destroy monitors (add, remove, orphan expiry). It is held
 *    across IddCxMonitorArrival/Departure, and never taken by an IddCx callback.
 *  - table_lock_ protects the monitor table and is held only briefly, never across an IddCx call, because
 *    IddCxMonitorArrival calls back into the driver (mode queries, swap-chain assignment) synchronously.
 */
#pragma once

#include <windows.h>
#include <wdf.h>
#include <iddcx.h>

#include "device_cache.h"
#include "swapchain_worker.h"
#include "tessera/display_modes.h"
#include "tessera_vdd_ioctl.h"

namespace tessera::vdd {

  class adapter_t;

  /// WDF context of each IDDCX_MONITOR object: which table slot (and which use of it) the monitor is.
  struct monitor_context_t {
    adapter_t *adapter;
    unsigned slot;
    unsigned generation;
  };

  WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(monitor_context_t, get_monitor_context);

  class adapter_t {
  public:
    explicit adapter_t(WDFDEVICE device);
    ~adapter_t();

    adapter_t(const adapter_t &) = delete;
    adapter_t &operator=(const adapter_t &) = delete;

    /// Creates the orphan timer. Called once from EvtDeviceAdd.
    NTSTATUS initialize();

    /// Starts IddCx adapter initialisation (D0 entry).
    NTSTATUS init_adapter();

    // --- control requests (EvtIddCxDeviceIoControl) ---
    void get_version(tessera_vdd_version_t &out);
    NTSTATUS set_render_adapter(const LUID &luid);
    void add_monitors(WDFFILEOBJECT owner, const tessera_vdd_add_request_t &in, tessera_vdd_add_reply_t &out);
    void remove_monitors(WDFFILEOBJECT owner, const tessera_vdd_remove_request_t &in, tessera_vdd_remove_reply_t &out);
    void list_monitors(WDFFILEOBJECT caller, tessera_vdd_list_reply_t &out);

    /// A handle was closed: its monitors are orphaned and removed after the grace period.
    void file_cleanup(WDFFILEOBJECT file);

    /// Removes orphaned monitors whose grace period has ended (orphan timer).
    void expire_orphans();

    // --- IddCx callbacks ---
    NTSTATUS adapter_init_finished(IDDCX_ADAPTER adapter, NTSTATUS init_status);
    NTSTATUS parse_monitor_description(const IDDCX_MONITOR_DESCRIPTION &description, UINT capacity, IDDCX_MONITOR_MODE *modes, IDDCX_MONITOR_MODE2 *modes2, IDARG_OUT_PARSEMONITORDESCRIPTION &out);
    NTSTATUS default_modes(IDDCX_MONITOR monitor, UINT capacity, IDDCX_MONITOR_MODE *modes, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES &out);
    NTSTATUS query_target_modes(IDDCX_MONITOR monitor, UINT capacity, IDDCX_TARGET_MODE *modes, IDDCX_TARGET_MODE2 *modes2, IDARG_OUT_QUERYTARGETMODES &out);
    NTSTATUS query_target_info(UINT connector, IDARG_OUT_QUERYTARGET_INFO &out);
    NTSTATUS assign_swapchain(IDDCX_MONITOR monitor, const IDARG_IN_SETSWAPCHAIN &in);
    NTSTATUS unassign_swapchain(IDDCX_MONITOR monitor);
    void monitor_destroyed(const monitor_context_t &ctx);

    /// IddCx >= 1.10 (HDR callbacks, FP16, ReleaseAndAcquireBuffer2).
    bool hdr_capable() const {
      return hdr_capable_;
    }

  private:
    enum class state_e {
      free,
      creating,  ///< reserved; IddCxMonitorCreate/Arrival in progress
      active,
      departing,
    };

    struct monitor_t {
      state_e state;
      unsigned generation;
      tessera_vdd_monitor_t desc;
      modes::mode_list_t modes;
      modes::edid_t edid;
      modes::edid_identity_t identity;
      unsigned connector;
      bool hdr;
      IDDCX_MONITOR object;
      WDFFILEOBJECT owner;  ///< nullptr when orphaned
      ULONGLONG orphaned_at;  ///< GetTickCount64() when the owner closed
      unsigned target_id;
      LUID os_adapter;
      swapchain_worker_t *worker;
    };

    static constexpr unsigned max_monitors = TESSERA_VDD_MAX_MONITORS;

    NTSTATUS add_one(WDFFILEOBJECT owner, const tessera_vdd_monitor_t &desc, tessera_vdd_add_result_t &result);
    NTSTATUS remove_slot(unsigned slot);
    int find_by_id(const unsigned char (&id)[16]) const;
    monitor_t *slot_of(IDDCX_MONITOR monitor);
    void schedule_orphan_timer();

    WDFDEVICE device_;
    IDDCX_ADAPTER adapter_;
    bool adapter_ready_;
    bool hdr_capable_;
    ULONG iddcx_version_;
    WDFTIMER orphan_timer_;
    SRWLOCK op_lock_;
    SRWLOCK table_lock_;
    monitor_t monitors_[max_monitors];
    bool render_adapter_set_;
    LUID render_adapter_;
    device_cache_t devices_;
  };

  /// The adapter_t of a WDF device (or of the IddCx adapter object, which shares it).
  struct device_context_t {
    adapter_t *adapter;
  };

  WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(device_context_t, get_device_context);

}  // namespace tessera::vdd
