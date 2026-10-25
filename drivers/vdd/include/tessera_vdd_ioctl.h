/**
 * @file drivers/vdd/include/tessera_vdd_ioctl.h
 * @brief Control interface between the Tessera host service and the Tessera virtual display driver.
 *
 * Shared by the driver (MSVC/WDK) and the host (MinGW), so it is plain C with fixed-size fields.
 * Every request and response struct starts with its own size; the driver rejects unknown sizes.
 * Access to the device interface is restricted to SYSTEM and Administrators by the INF.
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
#define TESSERA_VDD_INTERFACE_VERSION 1u

/// Returns tessera_vdd_version_t.
#define IOCTL_TESSERA_VDD_GET_VERSION CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_READ_ACCESS)

typedef struct tessera_vdd_version_t {
  unsigned int size;  ///< sizeof(tessera_vdd_version_t), set by the driver
  unsigned int interface_version;  ///< TESSERA_VDD_INTERFACE_VERSION
  unsigned int driver_major;
  unsigned int driver_minor;
  unsigned int driver_patch;
  unsigned int max_monitors;  ///< Monitors this adapter accepts
  unsigned int active_monitors;
} tessera_vdd_version_t;

#ifdef __cplusplus
}
#endif
