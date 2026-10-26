/**
 * @file drivers/vdd/src/device_cache.h
 * @brief One Direct3D 11 device per render adapter, shared by all monitors on that GPU.
 *
 * Creating a device per swap-chain assignment (as SudoVDA does) makes every topology change cost one device per
 * monitor, inside the IddCx callback. Here a device is created once per adapter LUID, lazily on a swap-chain worker
 * thread, and re-created only after the GPU removed it.
 */
#pragma once

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

namespace tessera::vdd {

  class device_cache_t {
  public:
    device_cache_t();
    ~device_cache_t();

    device_cache_t(const device_cache_t &) = delete;
    device_cache_t &operator=(const device_cache_t &) = delete;

    /// Returns the DXGI device for the render adapter, creating it on first use.
    HRESULT get(const LUID &adapter, Microsoft::WRL::ComPtr<IDXGIDevice> &out);

    /// Drops the cached device of this adapter if it is `broken` (e.g. after DXGI_ERROR_DEVICE_REMOVED).
    void invalidate(const LUID &adapter, IDXGIDevice *broken);

    void clear();

  private:
    static constexpr unsigned max_adapters = 8;

    struct entry_t {
      LUID luid;
      Microsoft::WRL::ComPtr<IDXGIDevice> device;
    };

    HRESULT create(const LUID &adapter, Microsoft::WRL::ComPtr<IDXGIDevice> &out);

    SRWLOCK lock_;
    entry_t entries_[max_adapters];
    unsigned count_;
  };

  inline bool same_luid(const LUID &a, const LUID &b) {
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
  }

}  // namespace tessera::vdd
