/**
 * @file drivers/vdd/src/swapchain_worker.cpp
 * @brief Swap-chain worker thread (see swapchain_worker.h).
 */
#include "swapchain_worker.h"

#include <avrt.h>

#include <new>

#include "trace.h"

using Microsoft::WRL::ComPtr;

namespace tessera::vdd {

  swapchain_worker_t::swapchain_worker_t(IDDCX_SWAPCHAIN swapchain, const LUID &render_adapter, HANDLE new_frame, device_cache_t &devices, bool use_buffer2):
      swapchain_(swapchain),
      render_adapter_(render_adapter),
      new_frame_(new_frame),
      terminate_(nullptr),
      thread_(nullptr),
      devices_(devices),
      use_buffer2_(use_buffer2),
      delete_swapchain_(1) {}

  swapchain_worker_t::~swapchain_worker_t() {
    if (thread_ != nullptr) {
      CloseHandle(thread_);
    }
    if (terminate_ != nullptr) {
      CloseHandle(terminate_);
    }
  }

  swapchain_worker_t *swapchain_worker_t::start(IDDCX_SWAPCHAIN swapchain, const LUID &render_adapter, HANDLE new_frame, device_cache_t &devices, bool use_buffer2) {
    auto *w = new (std::nothrow) swapchain_worker_t(swapchain, render_adapter, new_frame, devices, use_buffer2);
    if (w != nullptr) {
      w->terminate_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (w->terminate_ != nullptr) {
        w->thread_ = CreateThread(nullptr, 0, thread_main, w, 0, nullptr);
      }
      if (w->thread_ == nullptr) {
        log_status("swapchain_worker_t::start", STATUS_INSUFFICIENT_RESOURCES);
        delete w;
        w = nullptr;
      }
    }
    if (w == nullptr) {
      WdfObjectDelete(swapchain);  // Windows assigns a new swap chain later
    }
    return w;
  }

  void swapchain_worker_t::stop(bool delete_swapchain) {
    InterlockedExchange(&delete_swapchain_, delete_swapchain ? 1 : 0);
    SetEvent(terminate_);
    const ULONGLONG t0 = GetTickCount64();
    WaitForSingleObject(thread_, INFINITE);  // the thread never blocks outside its {frame, terminate} wait
    const ULONGLONG waited = GetTickCount64() - t0;
    if (waited > 100) {
      TraceLoggingWrite(g_trace, "SlowWorkerStop", TraceLoggingLevel(WINEVENT_LEVEL_WARNING), TraceLoggingUInt64(waited, "Milliseconds"));
    }
    delete this;
  }

  DWORD WINAPI swapchain_worker_t::thread_main(void *self) {
    static_cast<swapchain_worker_t *>(self)->run();
    return 0;
  }

  void swapchain_worker_t::run() {
    DWORD task_index = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"DisplayPostProcessing", &task_index);

    const char *exit_reason = "terminated";
    HRESULT exit_hr = S_OK;
    ComPtr<IDXGIDevice> device;
    HRESULT hr = devices_.get(render_adapter_, device);
    if (FAILED(hr)) {
      exit_reason = "no D3D device";
      exit_hr = hr;
    } else {
      IDARG_IN_SWAPCHAINSETDEVICE set = {};
      set.pDevice = device.Get();
      hr = IddCxSwapChainSetDevice(swapchain_, &set);
      if (FAILED(hr)) {
        exit_reason = "IddCxSwapChainSetDevice";
        exit_hr = hr;
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
          devices_.invalidate(render_adapter_, device.Get());
        }
      }
    }

    const HANDLE waits[2] = {new_frame_, terminate_};
    while (SUCCEEDED(hr)) {
      IDXGIResource *surface = nullptr;
      if (use_buffer2_) {
        IDARG_IN_RELEASEANDACQUIREBUFFER2 in = {};
        in.Size = sizeof(in);
        in.AcquireSystemMemoryBuffer = FALSE;
        IDARG_OUT_RELEASEANDACQUIREBUFFER2 out = {};
        hr = IddCxSwapChainReleaseAndAcquireBuffer2(swapchain_, &in, &out);
        surface = out.MetaData.pSurface;
      } else {
        IDARG_OUT_RELEASEANDACQUIREBUFFER out = {};
        hr = IddCxSwapChainReleaseAndAcquireBuffer(swapchain_, &out);
        surface = out.MetaData.pSurface;
      }

      if (hr == E_PENDING) {
        hr = S_OK;
        const DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0) {
          continue;
        }
        if (w != WAIT_OBJECT_0 + 1) {
          exit_reason = "wait failed";
          exit_hr = HRESULT_FROM_WIN32(GetLastError());
        }
        break;
      }
      if (FAILED(hr)) {
        exit_reason = "IddCxSwapChainReleaseAndAcquireBuffer";  // e.g. the swap chain was abandoned
        exit_hr = hr;
        break;
      }

      // Phase 1: the frame is not consumed. The acquired surface carries a reference for the driver.
      if (surface != nullptr) {
        surface->Release();
      }
      hr = IddCxSwapChainFinishedProcessingFrame(swapchain_);
      if (FAILED(hr)) {
        exit_reason = "IddCxSwapChainFinishedProcessingFrame";
        exit_hr = hr;
        break;
      }
      if (WaitForSingleObject(terminate_, 0) == WAIT_OBJECT_0) {
        break;  // stop promptly even while frames keep arriving
      }
    }

    device.Reset();
    // Deleting the swap chain tells Windows the driver is done with it; after a failure that also makes Windows
    // assign a new one.
    if (InterlockedCompareExchange(&delete_swapchain_, 0, 0) != 0) {
      WdfObjectDelete(swapchain_);
    }
    if (mmcss != nullptr) {
      AvRevertMmThreadCharacteristics(mmcss);
    }
    TraceLoggingWrite(g_trace, "WorkerExit", TraceLoggingString(exit_reason, "Reason"), TraceLoggingHResult(exit_hr, "HResult"));
  }

}  // namespace tessera::vdd
