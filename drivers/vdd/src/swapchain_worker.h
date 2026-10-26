/**
 * @file drivers/vdd/src/swapchain_worker.h
 * @brief One thread per monitor that drains the monitor's IddCx swap chain.
 *
 * Design rules (docs/phase-1-design.md, docs/vdd-freeze-analysis.md):
 *  - one worker per monitor; nothing is shared between monitors except the device cache;
 *  - the thread waits on {new frame, terminate} with no timeout: no polling, no spinning;
 *  - the only blocking call is that wait, so stop() always returns promptly;
 *  - no logging, file or registry I/O per frame;
 *  - MMCSS "DisplayPostProcessing"; the process priority is left alone.
 * Phase 1 releases every frame straight away (the host captures with DDA or WGC).
 */
#pragma once

#include <windows.h>
#include <wdf.h>
#include <iddcx.h>

#include "device_cache.h"

namespace tessera::vdd {

  class swapchain_worker_t {
  public:
    /// Starts a worker for a newly assigned swap chain. On failure the swap chain is deleted (so Windows assigns a
    /// new one later) and nullptr is returned.
    static swapchain_worker_t *start(IDDCX_SWAPCHAIN swapchain, const LUID &render_adapter, HANDLE new_frame, device_cache_t &devices, bool use_buffer2);

    /// Stops the thread and frees the worker. `delete_swapchain` is false when Windows has already destroyed the
    /// swap chain together with its monitor.
    void stop(bool delete_swapchain);

    const LUID &render_adapter() const {
      return render_adapter_;
    }

  private:
    swapchain_worker_t(IDDCX_SWAPCHAIN swapchain, const LUID &render_adapter, HANDLE new_frame, device_cache_t &devices, bool use_buffer2);
    ~swapchain_worker_t();

    static DWORD WINAPI thread_main(void *self);
    void run();

    IDDCX_SWAPCHAIN swapchain_;
    LUID render_adapter_;
    HANDLE new_frame_;  ///< Owned by IddCx.
    HANDLE terminate_;
    HANDLE thread_;
    device_cache_t &devices_;
    bool use_buffer2_;
    volatile LONG delete_swapchain_;  ///< Read by the thread when it exits.
  };

}  // namespace tessera::vdd
