/**
 * @file drivers/vdd/include/tessera_vdd_ioctl.h
 * @brief Control interface between the Tessera host service and the Tessera virtual display driver.
 *
 * Shared by the driver (MSVC/WDK) and the host (MinGW), so it is plain C with fixed-size fields.
 * Every request and response struct starts with its own size; the driver rejects unknown sizes.
 * Access to the device interface is restricted to SYSTEM and Administrators by the INF.
 *
 * Interface version 2 (Phase 1, docs/phase-1-design.md):
 *  - Monitors are added and removed at runtime, up to TESSERA_VDD_MAX_MONITORS per adapter, several per request.
 *  - A monitor belongs to the file handle that added it. When that handle closes, the driver removes the monitor
 *    after TESSERA_VDD_ORPHAN_GRACE_MS unless another handle adds the same ID again first (it then takes the monitor
 *    over without a departure and arrival).
 *  - Status values are NTSTATUS codes.
 */
#pragma once

#include <guiddef.h>
#include <winioctl.h>

#ifdef __cplusplus
extern "C" {
#endif

// {79457109-AE56-4AE0-B1C7-AD388E5DA044}
DEFINE_GUID(GUID_DEVINTERFACE_TESSERA_VDD, 0x79457109, 0xae56, 0x4ae0, 0xb1, 0xc7, 0xad, 0x38, 0x8e, 0x5d, 0xa0, 0x44);

/// Bumped on any incompatible change to the requests below.
#define TESSERA_VDD_INTERFACE_VERSION 2u

/// Monitors one adapter accepts, and the most one request can carry.
#define TESSERA_VDD_MAX_MONITORS 16u

/// How long monitors of a closed handle stay before they are removed.
#define TESSERA_VDD_ORPHAN_GRACE_MS 2000u

/// UTF-16 code units in a monitor name (shown as the first 13 ASCII characters in the EDID).
#define TESSERA_VDD_NAME_CHARS 32u

/// Returns tessera_vdd_version_t.
#define IOCTL_TESSERA_VDD_GET_VERSION CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_READ_ACCESS)

/// tessera_vdd_render_adapter_t in. Applies to swap chains assigned from now on; existing ones are re-created.
#define IOCTL_TESSERA_VDD_SET_RENDER_ADAPTER CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED, FILE_WRITE_ACCESS)

/// tessera_vdd_add_request_t in, tessera_vdd_add_reply_t out.
#define IOCTL_TESSERA_VDD_ADD_MONITORS CTL_CODE(FILE_DEVICE_UNKNOWN, 0x902, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

/// tessera_vdd_remove_request_t in, tessera_vdd_remove_reply_t out. Only the owning handle can remove a monitor.
#define IOCTL_TESSERA_VDD_REMOVE_MONITORS CTL_CODE(FILE_DEVICE_UNKNOWN, 0x903, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

/// tessera_vdd_list_reply_t out: every monitor of the adapter.
#define IOCTL_TESSERA_VDD_LIST_MONITORS CTL_CODE(FILE_DEVICE_UNKNOWN, 0x904, METHOD_BUFFERED, FILE_READ_ACCESS)

/// tessera_vdd_version_t::flags
#define TESSERA_VDD_FLAG_HDR_CAPABLE 0x1u  ///< IddCx >= 1.10: monitors can be added with TESSERA_VDD_MONITOR_HDR.
#define TESSERA_VDD_FLAG_RENDER_ADAPTER_SET 0x2u  ///< A render adapter was set with SET_RENDER_ADAPTER.

typedef struct tessera_vdd_version_t {
  unsigned int size;  ///< sizeof(tessera_vdd_version_t), set by the driver
  unsigned int interface_version;  ///< TESSERA_VDD_INTERFACE_VERSION
  unsigned int driver_major;
  unsigned int driver_minor;
  unsigned int driver_patch;
  unsigned int max_monitors;  ///< Monitors this adapter accepts
  unsigned int active_monitors;
  // Interface version 2:
  unsigned int flags;  ///< TESSERA_VDD_FLAG_*
  unsigned int iddcx_version;  ///< IddCxGetVersion value the driver runs on, e.g. 0x1A00 = IddCx 1.10; 0 = unknown
  unsigned int render_adapter_luid_low;  ///< Valid with TESSERA_VDD_FLAG_RENDER_ADAPTER_SET
  int render_adapter_luid_high;
} tessera_vdd_version_t;

typedef struct tessera_vdd_render_adapter_t {
  unsigned int size;  ///< sizeof(tessera_vdd_render_adapter_t)
  unsigned int luid_low;
  int luid_high;
} tessera_vdd_render_adapter_t;

/// tessera_vdd_monitor_t::flags
#define TESSERA_VDD_MONITOR_HDR 0x1u  ///< Advertise HDR10 (BT.2020, PQ). Needs TESSERA_VDD_FLAG_HDR_CAPABLE.

typedef struct tessera_vdd_monitor_t {
  unsigned char id[16];  ///< Stable ID chosen by the host (per client monitor); Windows remembers layout and DPI by it
  unsigned int width;  ///< Preferred mode
  unsigned int height;
  unsigned int refresh_numerator;  ///< Refresh rate in Hz as a fraction: 60/1, 60000/1001 (59.94), 120000/1001
  unsigned int refresh_denominator;
  unsigned int flags;  ///< TESSERA_VDD_MONITOR_*
  unsigned int width_mm;  ///< Physical size reported in the EDID; 0 = derive 96 DPI from the preferred mode
  unsigned int height_mm;
  unsigned short name[TESSERA_VDD_NAME_CHARS];  ///< UTF-16, NUL-terminated if shorter
} tessera_vdd_monitor_t;

typedef struct tessera_vdd_add_request_t {
  unsigned int size;  ///< sizeof(tessera_vdd_add_request_t)
  unsigned int count;  ///< 1..TESSERA_VDD_MAX_MONITORS
  tessera_vdd_monitor_t monitors[TESSERA_VDD_MAX_MONITORS];
} tessera_vdd_add_request_t;

/// tessera_vdd_add_result_t::flags
#define TESSERA_VDD_ADDED_RECLAIMED 0x1u  ///< The monitor already existed (orphaned or owned by this handle); kept as is

typedef struct tessera_vdd_add_result_t {
  int status;  ///< NTSTATUS for this monitor
  unsigned int target_id;  ///< OS target ID (matches DISPLAYCONFIG target IDs), valid on success
  unsigned int adapter_luid_low;  ///< OS adapter LUID of the monitor, valid on success
  int adapter_luid_high;
  unsigned int flags;  ///< TESSERA_VDD_ADDED_*
} tessera_vdd_add_result_t;

typedef struct tessera_vdd_add_reply_t {
  unsigned int size;  ///< sizeof(tessera_vdd_add_reply_t), set by the driver
  unsigned int count;  ///< Same as the request
  tessera_vdd_add_result_t results[TESSERA_VDD_MAX_MONITORS];
} tessera_vdd_add_reply_t;

typedef struct tessera_vdd_remove_request_t {
  unsigned int size;  ///< sizeof(tessera_vdd_remove_request_t)
  unsigned int count;  ///< 1..TESSERA_VDD_MAX_MONITORS
  unsigned char ids[TESSERA_VDD_MAX_MONITORS][16];
} tessera_vdd_remove_request_t;

typedef struct tessera_vdd_remove_reply_t {
  unsigned int size;  ///< sizeof(tessera_vdd_remove_reply_t), set by the driver
  unsigned int count;
  int status[TESSERA_VDD_MAX_MONITORS];  ///< NTSTATUS per ID
} tessera_vdd_remove_reply_t;

/// tessera_vdd_list_entry_t::state
#define TESSERA_VDD_STATE_OWNED_BY_CALLER 0x1u  ///< Added through the handle that sent LIST
#define TESSERA_VDD_STATE_ORPHANED 0x2u  ///< Its handle closed; removed when the grace period ends
#define TESSERA_VDD_STATE_SWAPCHAIN 0x4u  ///< Windows has assigned a swap chain (the monitor is active)

typedef struct tessera_vdd_list_entry_t {
  tessera_vdd_monitor_t monitor;
  unsigned int target_id;
  unsigned int connector_index;
  unsigned int state;  ///< TESSERA_VDD_STATE_*
} tessera_vdd_list_entry_t;

typedef struct tessera_vdd_list_reply_t {
  unsigned int size;  ///< sizeof(tessera_vdd_list_reply_t), set by the driver
  unsigned int count;
  tessera_vdd_list_entry_t entries[TESSERA_VDD_MAX_MONITORS];
} tessera_vdd_list_reply_t;

#ifdef __cplusplus
}
#endif
