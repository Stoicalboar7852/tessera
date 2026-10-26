/**
 * @file drivers/vdd/src/trace.h
 * @brief TraceLogging for the Tessera VDD (provider "Tessera.Vdd", {9358308D-0904-440B-A1D9-2FB3E81370E5}).
 *
 * Events are written on state changes only, never per frame (docs/phase-1-design.md).
 * Capture: wpr -start <profile with the provider> or tracelog / logman with the GUID above.
 */
#pragma once

#include <windows.h>
#include <wdf.h>

#include <TraceLoggingProvider.h>
#include <winmeta.h>

TRACELOGGING_DECLARE_PROVIDER(g_trace);

namespace tessera::vdd {

  // The event level is part of the event's static metadata, so success and failure are separate writes.
  inline void log_status(const char *where, NTSTATUS status) {
    if (NT_SUCCESS(status)) {
      TraceLoggingWrite(g_trace, "Status", TraceLoggingLevel(WINEVENT_LEVEL_INFO), TraceLoggingString(where, "Where"), TraceLoggingNTStatus(status, "Status"));
    } else {
      TraceLoggingWrite(g_trace, "Status", TraceLoggingLevel(WINEVENT_LEVEL_ERROR), TraceLoggingString(where, "Where"), TraceLoggingNTStatus(status, "Status"));
    }
  }

  inline void log_hresult(const char *where, HRESULT hr) {
    if (SUCCEEDED(hr)) {
      TraceLoggingWrite(g_trace, "HResult", TraceLoggingLevel(WINEVENT_LEVEL_INFO), TraceLoggingString(where, "Where"), TraceLoggingHResult(hr, "HResult"));
    } else {
      TraceLoggingWrite(g_trace, "HResult", TraceLoggingLevel(WINEVENT_LEVEL_ERROR), TraceLoggingString(where, "Where"), TraceLoggingHResult(hr, "HResult"));
    }
  }

}  // namespace tessera::vdd
