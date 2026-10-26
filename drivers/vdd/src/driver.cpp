/**
 * @file drivers/vdd/src/driver.cpp
 * @brief Tessera virtual display driver (UMDF 2 + IddCx): driver entry, device setup, control requests and the
 *        IddCx callback table.
 *
 * Monitors are added and removed at runtime through the control interface (tessera_vdd_ioctl.h); the design and its
 * reasons are in docs/phase-1-design.md and docs/vdd-freeze-analysis.md.
 * Written from the IddCx documentation (not derived from the MS-PL sample), GPLv3: docs/adr/0005-vdd-driver.md.
 * No exceptions: failures are NTSTATUS values, logged through TraceLogging (provider "Tessera.Vdd").
 */
#include <windows.h>
#include <wdf.h>
#include <iddcx.h>

#include <new>

#include <initguid.h>  // instantiate the interface GUID in this translation unit
#include "tessera_vdd_ioctl.h"

#include "adapter.h"
#include "trace.h"

// {9358308D-0904-440B-A1D9-2FB3E81370E5}
TRACELOGGING_DEFINE_PROVIDER(g_trace, "Tessera.Vdd", (0x9358308d, 0x0904, 0x440b, 0xa1, 0xd9, 0x2f, 0xb3, 0xe8, 0x13, 0x70, 0xe5));

using namespace tessera::vdd;

namespace {

  adapter_t *adapter_of(WDFOBJECT object) {
    const device_context_t *ctx = get_device_context(object);
    return ctx != nullptr ? ctx->adapter : nullptr;
  }

  adapter_t *adapter_of_monitor(IDDCX_MONITOR monitor) {
    const monitor_context_t *ctx = get_monitor_context(monitor);
    return ctx != nullptr ? ctx->adapter : nullptr;
  }

  // --- control requests --------------------------------------------------------------------------------------

  /// Copies a fixed-size, size-prefixed request out of the request's input buffer.
  template<class T>
  NTSTATUS read_input(WDFREQUEST request, size_t input_length, T &out) {
    if (input_length < sizeof(T)) {
      return STATUS_BUFFER_TOO_SMALL;
    }
    void *buffer = nullptr;
    const NTSTATUS status = WdfRequestRetrieveInputBuffer(request, sizeof(T), &buffer, nullptr);
    if (!NT_SUCCESS(status)) {
      return status;
    }
    // Copy before anything is written back: buffered requests may share one buffer for input and output.
    memcpy(&out, buffer, sizeof(T));
    return out.size == sizeof(T) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
  }

  template<class T>
  NTSTATUS write_output(WDFREQUEST request, size_t output_length, const T &reply, size_t &written) {
    if (output_length < sizeof(T)) {
      return STATUS_BUFFER_TOO_SMALL;
    }
    void *buffer = nullptr;
    const NTSTATUS status = WdfRequestRetrieveOutputBuffer(request, sizeof(T), &buffer, nullptr);
    if (NT_SUCCESS(status)) {
      memcpy(buffer, &reply, sizeof(T));
      written = sizeof(T);
    }
    return status;
  }

  void evt_device_io_control(WDFDEVICE device, WDFREQUEST request, size_t output_length, size_t input_length, ULONG code) {
    adapter_t *adapter = adapter_of(device);
    const WDFFILEOBJECT file = WdfRequestGetFileObject(request);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    size_t written = 0;
    if (adapter == nullptr) {
      status = STATUS_DEVICE_NOT_READY;
    } else if (code == IOCTL_TESSERA_VDD_GET_VERSION) {
      // Interface 1 callers know only the first seven fields; give them what fits.
      tessera_vdd_version_t v;
      adapter->get_version(v);
      constexpr size_t v1_size = 7 * sizeof(unsigned int);
      const size_t n = output_length >= sizeof(v) ? sizeof(v) : (output_length >= v1_size ? v1_size : 0);
      void *buffer = nullptr;
      status = n == 0 ? STATUS_BUFFER_TOO_SMALL : WdfRequestRetrieveOutputBuffer(request, n, &buffer, nullptr);
      if (NT_SUCCESS(status)) {
        v.size = static_cast<unsigned int>(n);
        memcpy(buffer, &v, n);
        written = n;
      }
    } else if (file == nullptr) {
      status = STATUS_INVALID_DEVICE_STATE;  // monitors are owned by a handle; requests without one are refused
    } else if (code == IOCTL_TESSERA_VDD_SET_RENDER_ADAPTER) {
      tessera_vdd_render_adapter_t in;
      status = read_input(request, input_length, in);
      if (NT_SUCCESS(status)) {
        LUID luid;
        luid.LowPart = in.luid_low;
        luid.HighPart = in.luid_high;
        status = adapter->set_render_adapter(luid);
      }
    } else if (code == IOCTL_TESSERA_VDD_ADD_MONITORS) {
      auto *in = new (std::nothrow) tessera_vdd_add_request_t;
      auto *out = new (std::nothrow) tessera_vdd_add_reply_t;
      status = (in == nullptr || out == nullptr) ? STATUS_INSUFFICIENT_RESOURCES : read_input(request, input_length, *in);
      if (NT_SUCCESS(status) && (in->count == 0 || in->count > TESSERA_VDD_MAX_MONITORS)) {
        status = STATUS_INVALID_PARAMETER;
      }
      if (NT_SUCCESS(status) && output_length < sizeof(*out)) {
        status = STATUS_BUFFER_TOO_SMALL;
      }
      if (NT_SUCCESS(status)) {
        adapter->add_monitors(file, *in, *out);
        status = write_output(request, output_length, *out, written);
      }
      delete in;
      delete out;
    } else if (code == IOCTL_TESSERA_VDD_REMOVE_MONITORS) {
      tessera_vdd_remove_request_t in;
      tessera_vdd_remove_reply_t out;
      status = read_input(request, input_length, in);
      if (NT_SUCCESS(status) && (in.count == 0 || in.count > TESSERA_VDD_MAX_MONITORS)) {
        status = STATUS_INVALID_PARAMETER;
      }
      if (NT_SUCCESS(status) && output_length < sizeof(out)) {
        status = STATUS_BUFFER_TOO_SMALL;
      }
      if (NT_SUCCESS(status)) {
        adapter->remove_monitors(file, in, out);
        status = write_output(request, output_length, out, written);
      }
    } else if (code == IOCTL_TESSERA_VDD_LIST_MONITORS) {
      auto *out = new (std::nothrow) tessera_vdd_list_reply_t;
      status = out == nullptr ? STATUS_INSUFFICIENT_RESOURCES : STATUS_SUCCESS;
      if (NT_SUCCESS(status)) {
        adapter->list_monitors(file, *out);
        status = write_output(request, output_length, *out, written);
      }
      delete out;
    }
    WdfRequestCompleteWithInformation(request, status, written);
  }

  void evt_file_cleanup(WDFFILEOBJECT file) {
    adapter_t *adapter = adapter_of(WdfFileObjectGetDevice(file));
    if (adapter != nullptr) {
      adapter->file_cleanup(file);
    }
  }

  // --- IddCx callbacks ---------------------------------------------------------------------------------------

  EVT_IDD_CX_ADAPTER_INIT_FINISHED evt_adapter_init_finished;
  EVT_IDD_CX_ADAPTER_COMMIT_MODES evt_adapter_commit_modes;
  EVT_IDD_CX_ADAPTER_COMMIT_MODES2 evt_adapter_commit_modes2;
  EVT_IDD_CX_ADAPTER_QUERY_TARGET_INFO evt_adapter_query_target_info;
  EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION evt_parse_monitor_description;
  EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION2 evt_parse_monitor_description2;
  EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES evt_monitor_get_default_modes;
  EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES evt_monitor_query_target_modes;
  EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES2 evt_monitor_query_target_modes2;
  EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN evt_monitor_assign_swapchain;
  EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN evt_monitor_unassign_swapchain;
  EVT_IDD_CX_MONITOR_SET_DEFAULT_HDR_METADATA evt_monitor_set_default_hdr_metadata;
  EVT_IDD_CX_MONITOR_SET_GAMMA_RAMP evt_monitor_set_gamma_ramp;

  // The parse callbacks get no device; there is one Tessera adapter per driver host (own DeviceGroupId).
  adapter_t *g_adapter = nullptr;

  NTSTATUS evt_adapter_init_finished(IDDCX_ADAPTER adapter_object, const IDARG_IN_ADAPTER_INIT_FINISHED *args) {
    adapter_t *adapter = adapter_of(adapter_object);
    return adapter != nullptr ? adapter->adapter_init_finished(adapter_object, args->AdapterInitStatus) : STATUS_INVALID_DEVICE_STATE;
  }

  NTSTATUS evt_adapter_commit_modes(IDDCX_ADAPTER, const IDARG_IN_COMMITMODES *) {
    return STATUS_SUCCESS;  // nothing to program: the swap chain always has the committed mode's size
  }

  NTSTATUS evt_adapter_commit_modes2(IDDCX_ADAPTER, const IDARG_IN_COMMITMODES2 *) {
    return STATUS_SUCCESS;
  }

  NTSTATUS evt_adapter_query_target_info(IDDCX_ADAPTER adapter_object, IDARG_IN_QUERYTARGET_INFO *in, IDARG_OUT_QUERYTARGET_INFO *out) {
    adapter_t *adapter = adapter_of(adapter_object);
    return adapter != nullptr ? adapter->query_target_info(in->ConnectorIndex, *out) : STATUS_INVALID_DEVICE_STATE;
  }

  NTSTATUS evt_parse_monitor_description(const IDARG_IN_PARSEMONITORDESCRIPTION *in, IDARG_OUT_PARSEMONITORDESCRIPTION *out) {
    return g_adapter != nullptr ? g_adapter->parse_monitor_description(in->MonitorDescription, in->MonitorModeBufferInputCount, in->pMonitorModes, nullptr, *out) : STATUS_INVALID_DEVICE_STATE;
  }

  NTSTATUS evt_parse_monitor_description2(const IDARG_IN_PARSEMONITORDESCRIPTION2 *in, IDARG_OUT_PARSEMONITORDESCRIPTION *out) {
    return g_adapter != nullptr ? g_adapter->parse_monitor_description(in->MonitorDescription, in->MonitorModeBufferInputCount, nullptr, in->pMonitorModes, *out) : STATUS_INVALID_DEVICE_STATE;
  }

  NTSTATUS evt_monitor_get_default_modes(IDDCX_MONITOR monitor, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES *in, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES *out) {
    adapter_t *adapter = adapter_of_monitor(monitor);
    return adapter != nullptr ? adapter->default_modes(monitor, in->DefaultMonitorModeBufferInputCount, in->pDefaultMonitorModes, *out) : STATUS_INVALID_DEVICE_STATE;
  }

  NTSTATUS evt_monitor_query_target_modes(IDDCX_MONITOR monitor, const IDARG_IN_QUERYTARGETMODES *in, IDARG_OUT_QUERYTARGETMODES *out) {
    adapter_t *adapter = adapter_of_monitor(monitor);
    return adapter != nullptr ? adapter->query_target_modes(monitor, in->TargetModeBufferInputCount, in->pTargetModes, nullptr, *out) : STATUS_INVALID_DEVICE_STATE;
  }

  NTSTATUS evt_monitor_query_target_modes2(IDDCX_MONITOR monitor, const IDARG_IN_QUERYTARGETMODES2 *in, IDARG_OUT_QUERYTARGETMODES *out) {
    adapter_t *adapter = adapter_of_monitor(monitor);
    return adapter != nullptr ? adapter->query_target_modes(monitor, in->TargetModeBufferInputCount, nullptr, in->pTargetModes, *out) : STATUS_INVALID_DEVICE_STATE;
  }

  NTSTATUS evt_monitor_assign_swapchain(IDDCX_MONITOR monitor, const IDARG_IN_SETSWAPCHAIN *in) {
    adapter_t *adapter = adapter_of_monitor(monitor);
    return adapter != nullptr ? adapter->assign_swapchain(monitor, *in) : STATUS_INVALID_DEVICE_STATE;
  }

  NTSTATUS evt_monitor_unassign_swapchain(IDDCX_MONITOR monitor) {
    adapter_t *adapter = adapter_of_monitor(monitor);
    return adapter != nullptr ? adapter->unassign_swapchain(monitor) : STATUS_SUCCESS;
  }

  NTSTATUS evt_monitor_set_default_hdr_metadata(IDDCX_MONITOR, const IDARG_IN_MONITOR_SET_DEFAULT_HDR_METADATA *) {
    return STATUS_SUCCESS;  // frames are not consumed in Phase 1, so there is no metadata to apply
  }

  NTSTATUS evt_monitor_set_gamma_ramp(IDDCX_MONITOR, const IDARG_IN_SET_GAMMARAMP *) {
    return STATUS_SUCCESS;
  }

  // --- device ------------------------------------------------------------------------------------------------

  NTSTATUS evt_device_d0_entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous_state) {
    UNREFERENCED_PARAMETER(previous_state);
    adapter_t *adapter = adapter_of(device);
    return adapter != nullptr ? adapter->init_adapter() : STATUS_INVALID_DEVICE_STATE;
  }

  void evt_device_cleanup(WDFOBJECT object) {
    device_context_t *ctx = get_device_context(object);
    if (ctx != nullptr && ctx->adapter != nullptr) {
      if (g_adapter == ctx->adapter) {
        g_adapter = nullptr;
      }
      delete ctx->adapter;
      ctx->adapter = nullptr;
    }
  }

  NTSTATUS evt_device_add(WDFDRIVER driver, PWDFDEVICE_INIT init) {
    UNREFERENCED_PARAMETER(driver);

    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDeviceD0Entry = evt_device_d0_entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);

    // File objects carry monitor ownership: closing a handle orphans its monitors.
    WDF_FILEOBJECT_CONFIG file_config;
    WDF_FILEOBJECT_CONFIG_INIT(&file_config, nullptr, nullptr, evt_file_cleanup);
    WdfDeviceInitSetFileObjectConfig(init, &file_config, WDF_NO_OBJECT_ATTRIBUTES);

    IDD_CX_CLIENT_CONFIG config;
    IDD_CX_CLIENT_CONFIG_INIT(&config);
    config.EvtIddCxDeviceIoControl = evt_device_io_control;
    config.EvtIddCxAdapterInitFinished = evt_adapter_init_finished;
    config.EvtIddCxMonitorGetDefaultDescriptionModes = evt_monitor_get_default_modes;
    config.EvtIddCxMonitorAssignSwapChain = evt_monitor_assign_swapchain;
    config.EvtIddCxMonitorUnassignSwapChain = evt_monitor_unassign_swapchain;
    if (IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxAdapterQueryTargetInfo)) {
      // IddCx 1.10+: the HDR-capable callback set replaces the older one.
      config.EvtIddCxParseMonitorDescription2 = evt_parse_monitor_description2;
      config.EvtIddCxMonitorQueryTargetModes2 = evt_monitor_query_target_modes2;
      config.EvtIddCxAdapterCommitModes2 = evt_adapter_commit_modes2;
      config.EvtIddCxAdapterQueryTargetInfo = evt_adapter_query_target_info;
      config.EvtIddCxMonitorSetDefaultHdrMetaData = evt_monitor_set_default_hdr_metadata;
      config.EvtIddCxMonitorSetGammaRamp = evt_monitor_set_gamma_ramp;
    } else {
      config.EvtIddCxParseMonitorDescription = evt_parse_monitor_description;
      config.EvtIddCxMonitorQueryTargetModes = evt_monitor_query_target_modes;
      config.EvtIddCxAdapterCommitModes = evt_adapter_commit_modes;
    }

    NTSTATUS status = IddCxDeviceInitConfig(init, &config);
    if (!NT_SUCCESS(status)) {
      log_status("IddCxDeviceInitConfig", status);
      return status;
    }

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, device_context_t);
    attributes.EvtCleanupCallback = evt_device_cleanup;
    WDFDEVICE device = nullptr;
    status = WdfDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(status)) {
      log_status("WdfDeviceCreate", status);
      return status;
    }
    device_context_t *ctx = get_device_context(device);
    ctx->adapter = new (std::nothrow) adapter_t(device);
    if (ctx->adapter == nullptr) {
      return STATUS_INSUFFICIENT_RESOURCES;
    }
    g_adapter = ctx->adapter;

    status = ctx->adapter->initialize();
    if (!NT_SUCCESS(status)) {
      return status;
    }
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
