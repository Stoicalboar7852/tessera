/**
 * @file drivers/vdd/src/device_cache.cpp
 * @brief Per-render-adapter Direct3D 11 devices (see device_cache.h).
 */
#include "device_cache.h"

#include <utility>

#include "trace.h"

using Microsoft::WRL::ComPtr;

namespace tessera::vdd {

  device_cache_t::device_cache_t():
      lock_(SRWLOCK_INIT),
      entries_(),
      count_(0) {}

  device_cache_t::~device_cache_t() {
    clear();
  }

  HRESULT device_cache_t::get(const LUID &adapter, ComPtr<IDXGIDevice> &out) {
    AcquireSRWLockExclusive(&lock_);
    HRESULT hr = S_OK;
    unsigned i = 0;
    for (; i < count_; ++i) {
      if (same_luid(entries_[i].luid, adapter)) {
        break;
      }
    }
    if (i < count_) {
      out = entries_[i].device;
      ++entries_[i].users;
    } else {
      // Created under the lock: monitors on the same GPU wait for the one device instead of each creating its own.
      hr = create(adapter, out);
      if (SUCCEEDED(hr)) {
        if (count_ < max_adapters) {
          entries_[count_].luid = adapter;
          entries_[count_].device = out;
          entries_[count_].users = 1;
          ++count_;
        }
        TraceLoggingWrite(g_trace, "DeviceCreated", TraceLoggingUInt32(adapter.LowPart, "LuidLow"), TraceLoggingInt32(adapter.HighPart, "LuidHigh"));
      }
    }
    ReleaseSRWLockExclusive(&lock_);
    return hr;
  }

  void device_cache_t::release(const LUID &adapter, IDXGIDevice *device) {
    ComPtr<IDXGIDevice> last;  // destroyed outside the lock
    AcquireSRWLockExclusive(&lock_);
    for (unsigned i = 0; i < count_; ++i) {
      if (same_luid(entries_[i].luid, adapter) && entries_[i].device.Get() == device) {
        if (--entries_[i].users == 0) {
          last = std::move(entries_[i].device);
          entries_[i] = entries_[count_ - 1];
          entries_[count_ - 1].device.Reset();
          --count_;
        }
        break;
      }
    }
    ReleaseSRWLockExclusive(&lock_);
    if (last) {
      last.Reset();
      TraceLoggingWrite(g_trace, "DeviceReleased", TraceLoggingUInt32(adapter.LowPart, "LuidLow"), TraceLoggingInt32(adapter.HighPart, "LuidHigh"));
    }
  }

  void device_cache_t::invalidate(const LUID &adapter, IDXGIDevice *broken) {
    ComPtr<IDXGIDevice> dropped;
    AcquireSRWLockExclusive(&lock_);
    for (unsigned i = 0; i < count_; ++i) {
      if (same_luid(entries_[i].luid, adapter) && entries_[i].device.Get() == broken) {
        dropped = std::move(entries_[i].device);
        entries_[i] = entries_[count_ - 1];
        entries_[count_ - 1].device.Reset();
        --count_;
        break;
      }
    }
    ReleaseSRWLockExclusive(&lock_);
  }

  void device_cache_t::clear() {
    AcquireSRWLockExclusive(&lock_);
    for (unsigned i = 0; i < count_; ++i) {
      entries_[i].device.Reset();
    }
    count_ = 0;
    ReleaseSRWLockExclusive(&lock_);
  }

  HRESULT device_cache_t::create(const LUID &adapter, ComPtr<IDXGIDevice> &out) {
    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
      log_hresult("CreateDXGIFactory2", hr);
      return hr;
    }
    ComPtr<IDXGIAdapter> dxgi_adapter;
    hr = factory->EnumAdapterByLuid(adapter, IID_PPV_ARGS(&dxgi_adapter));
    if (FAILED(hr)) {
      log_hresult("EnumAdapterByLuid", hr);
      return hr;
    }
    ComPtr<ID3D11Device> device;
    // BGRA support: the swap-chain surfaces are B8G8R8A8 (or FP16 for HDR).
    hr = D3D11CreateDevice(dxgi_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr);
    if (FAILED(hr)) {
      log_hresult("D3D11CreateDevice", hr);
      return hr;
    }
    hr = device.As(&out);
    if (FAILED(hr)) {
      log_hresult("ID3D11Device::QueryInterface(IDXGIDevice)", hr);
    }
    return hr;
  }

}  // namespace tessera::vdd
