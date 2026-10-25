/**
 * @file drivers/vdd/src/driver.cpp
 * @brief Tessera virtual display driver (UMDF 2 + IddCx). Phase 0 skeleton.
 *
 * Phase 0 scope: proves that the toolchain, signing and installation work (including with Secure Boot on).
 * The driver loads, initialises one IddCx adapter that reports zero monitors, and answers
 * IOCTL_TESSERA_VDD_GET_VERSION. Monitors, modes and swap chains arrive in Phase 1.
 *
 * Written from the IddCx documentation (not derived from the MS-PL sample); license per docs/adr (Phase 1).
 * No exceptions: failures are NTSTATUS values, logged through TraceLogging (provider "Tessera.Vdd").
 */
#include <windows.h>
#include <wdf.h>
#include <iddcx.h>

#include <TraceLoggingProvider.h>

#include <initguid.h>  // instantiate the interface GUID in this translation unit
#include "tessera_vdd_ioctl.h"

// {9358308D-0904-440B-A1D9-2FB3E81370E5}
TRACELOGGING_DEFINE_PROVIDER(g_trace, "Tessera.Vdd", (0x9358308d, 0x0904, 0x440b, 0xa1, 0xd9, 0x2f, 0xb3, 0xe8, 0x13, 0x70, 0xe5));

#define TESSERA_VDD_VERSION_MAJOR 0u
#define TESSERA_VDD_VERSION_MINOR 1u
#define TESSERA_VDD_VERSION_PATCH 0u
#define TESSERA_VDD_MAX_MONITORS 16u

namespace {

  struct device_context_t {
    IDDCX_ADAPTER adapter;
    bool adapter_ready;
  };

  WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(device_context_t, get_device_context);

  void log_status(const char *where, NTSTATUS status) {
    TraceLoggingWrite(g_trace, "Status", TraceLoggingString(where, "Where"), TraceLoggingNTStatus(status, "Status"));
  }

  // Adapter caps must stay valid while IddCxAdapterInitAsync runs, so they live in static storage.
  IDDCX_ENDPOINT_VERSION g_endpoint_version = {sizeof(IDDCX_ENDPOINT_VERSION), TESSERA_VDD_VERSION_MAJOR, TESSERA_VDD_VERSION_MINOR, TESSERA_VDD_VERSION_PATCH, 0};

  NTSTATUS evt_adapter_init_finished(IDDCX_ADAPTER adapter, const IDARG_IN_ADAPTER_INIT_FINISHED *args) {
    UNREFERENCED_PARAMETER(adapter);
    log_status("AdapterInitFinished", args->AdapterInitStatus);
    return STATUS_SUCCESS;
  }

  NTSTATUS evt_adapter_commit_modes(IDDCX_ADAPTER adapter, const IDARG_IN_COMMITMODES *args) {
    UNREFERENCED_PARAMETER(adapter);
    UNREFERENCED_PARAMETER(args);
    return STATUS_SUCCESS;
  }

  // No monitors exist in Phase 0, so the monitor callbacks are never invoked; they refuse defensively.
  NTSTATUS evt_parse_monitor_description(const IDARG_IN_PARSEMONITORDESCRIPTION *in, IDARG_OUT_PARSEMONITORDESCRIPTION *out) {
    UNREFERENCED_PARAMETER(in);
    UNREFERENCED_PARAMETER(out);
    return STATUS_NOT_SUPPORTED;
  }

  NTSTATUS evt_monitor_get_default_modes(IDDCX_MONITOR monitor, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES *in, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES *out) {
    UNREFERENCED_PARAMETER(monitor);
    UNREFERENCED_PARAMETER(in);
    UNREFERENCED_PARAMETER(out);
    return STATUS_NOT_SUPPORTED;
  }

  NTSTATUS evt_monitor_query_target_modes(IDDCX_MONITOR monitor, const IDARG_IN_QUERYTARGETMODES *in, IDARG_OUT_QUERYTARGETMODES *out) {
    UNREFERENCED_PARAMETER(monitor);
    UNREFERENCED_PARAMETER(in);
    UNREFERENCED_PARAMETER(out);
    return STATUS_NOT_SUPPORTED;
  }

  NTSTATUS evt_monitor_assign_swapchain(IDDCX_MONITOR monitor, const IDARG_IN_SETSWAPCHAIN *in) {
    UNREFERENCED_PARAMETER(monitor);
    UNREFERENCED_PARAMETER(in);
    return STATUS_NOT_SUPPORTED;
  }

  NTSTATUS evt_monitor_unassign_swapchain(IDDCX_MONITOR monitor) {
    UNREFERENCED_PARAMETER(monitor);
    return STATUS_SUCCESS;
  }

  void evt_device_io_control(WDFDEVICE device, WDFREQUEST request, size_t output_length, size_t input_length, ULONG code) {
    UNREFERENCED_PARAMETER(input_length);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    size_t written = 0;
    if (code == IOCTL_TESSERA_VDD_GET_VERSION) {
      tessera_vdd_version_t *out = nullptr;
      status = output_length < sizeof(*out) ? STATUS_BUFFER_TOO_SMALL : WdfRequestRetrieveOutputBuffer(request, sizeof(*out), reinterpret_cast<PVOID *>(&out), nullptr);
      if (NT_SUCCESS(status)) {
        RtlZeroMemory(out, sizeof(*out));
        out->size = sizeof(*out);
        out->interface_version = TESSERA_VDD_INTERFACE_VERSION;
        out->driver_major = TESSERA_VDD_VERSION_MAJOR;
        out->driver_minor = TESSERA_VDD_VERSION_MINOR;
        out->driver_patch = TESSERA_VDD_VERSION_PATCH;
        out->max_monitors = TESSERA_VDD_MAX_MONITORS;
        out->active_monitors = 0;
        written = sizeof(*out);
      }
      UNREFERENCED_PARAMETER(device);
    }
    WdfRequestCompleteWithInformation(request, status, written);
  }

  NTSTATUS evt_device_d0_entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous_state) {
    UNREFERENCED_PARAMETER(previous_state);
    auto *ctx = get_device_context(device);
    if (ctx->adapter_ready) {
      return STATUS_SUCCESS;  // resuming from a low-power state; the adapter already exists
    }

    IDDCX_ADAPTER_CAPS caps = {};
    caps.Size = sizeof(caps);
    caps.MaxMonitorsSupported = TESSERA_VDD_MAX_MONITORS;
    caps.EndPointDiagnostics.Size = sizeof(caps.EndPointDiagnostics);
    caps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    caps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;
    caps.EndPointDiagnostics.pEndPointFriendlyName = L"Tessera Virtual Display";
    caps.EndPointDiagnostics.pEndPointManufacturerName = L"Tessera";
    caps.EndPointDiagnostics.pEndPointModelName = L"Tessera VDD";
    caps.EndPointDiagnostics.pFirmwareVersion = &g_endpoint_version;
    caps.EndPointDiagnostics.pHardwareVersion = &g_endpoint_version;

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

    IDARG_IN_ADAPTER_INIT in = {};
    in.WdfDevice = device;
    in.pCaps = &caps;
    in.ObjectAttributes = &attributes;
    IDARG_OUT_ADAPTER_INIT out = {};
    const NTSTATUS status = IddCxAdapterInitAsync(&in, &out);
    log_status("IddCxAdapterInitAsync", status);
    if (NT_SUCCESS(status)) {
      ctx->adapter = out.AdapterObject;
      ctx->adapter_ready = true;
    }
    return status;
  }

  NTSTATUS evt_device_add(WDFDRIVER driver, PWDFDEVICE_INIT init) {
    UNREFERENCED_PARAMETER(driver);

    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDeviceD0Entry = evt_device_d0_entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);

    IDD_CX_CLIENT_CONFIG config;
    IDD_CX_CLIENT_CONFIG_INIT(&config);
    config.EvtIddCxDeviceIoControl = evt_device_io_control;
    config.EvtIddCxAdapterInitFinished = evt_adapter_init_finished;
    config.EvtIddCxAdapterCommitModes = evt_adapter_commit_modes;
    config.EvtIddCxParseMonitorDescription = evt_parse_monitor_description;
    config.EvtIddCxMonitorGetDefaultDescriptionModes = evt_monitor_get_default_modes;
    config.EvtIddCxMonitorQueryTargetModes = evt_monitor_query_target_modes;
    config.EvtIddCxMonitorAssignSwapChain = evt_monitor_assign_swapchain;
    config.EvtIddCxMonitorUnassignSwapChain = evt_monitor_unassign_swapchain;

    NTSTATUS status = IddCxDeviceInitConfig(init, &config);
    if (!NT_SUCCESS(status)) {
      log_status("IddCxDeviceInitConfig", status);
      return status;
    }

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, device_context_t);
    WDFDEVICE device = nullptr;
    status = WdfDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(status)) {
      log_status("WdfDeviceCreate", status);
      return status;
    }
    auto *ctx = get_device_context(device);
    ctx->adapter = nullptr;
    ctx->adapter_ready = false;

    status = IddCxDeviceInitialize(device);
    if (!NT_SUCCESS(status)) {
      log_status("IddCxDeviceInitialize", status);
      return status;
    }

    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_TESSERA_VDD, nullptr);
    if (!NT_SUCCESS(status)) {
      log_status("WdfDeviceCreateDeviceInterface", status);
    }
    return status;
  }

  void evt_driver_cleanup(WDFOBJECT driver) {
    UNREFERENCED_PARAMETER(driver);
    TraceLoggingUnregister(g_trace);
  }

}  // namespace

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver_object, PUNICODE_STRING registry_path) {
  TraceLoggingRegister(g_trace);

  WDF_DRIVER_CONFIG config;
  WDF_DRIVER_CONFIG_INIT(&config, evt_device_add);

  WDF_OBJECT_ATTRIBUTES attributes;
  WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
  attributes.EvtCleanupCallback = evt_driver_cleanup;

  const NTSTATUS status = WdfDriverCreate(driver_object, registry_path, &attributes, &config, WDF_NO_HANDLE);
  log_status("WdfDriverCreate", status);
  if (!NT_SUCCESS(status)) {
    TraceLoggingUnregister(g_trace);
  }
  return status;
}
