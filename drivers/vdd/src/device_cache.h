/**
 * @file drivers/vdd/src/device_cache.h
 * @brief One Direct3D 11 device per render adapter, shared by all monitors on that GPU.
 *
 * Creating a device per swap-chain assignment (as SudoVDA does) makes every topology change cost one device per
 * monitor, inside the IddCx callback. Here a device is created once per adapter LUID, lazily on a swap-chain worker
 * thread, and shared by the workers using that GPU. It is released when the last of those workers is done with it:
 * a device must not outlive the swap chains it served (keeping it until the driver unloads made the device removal
 * hang on the dev PC, 2026-10-27).
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

    /// Returns the DXGI device for the render adapter, creating it on first use, and counts the caller as a user.
    /// Every successful get() must be paired with a release() once the caller no longer uses the device.
    HRESULT get(const LUID &adapter, Microsoft::WRL::ComPtr<IDXGIDevice> &out);

    /// Ends one user of `device`; the cache drops the device when its last user is gone.
    void release(const LUID &adapter, IDXGIDevice *device);

    /// Stops sharing a `broken` device (e.g. after DXGI_ERROR_DEVICE_REMOVED); its users still release() it.
    void invalidate(const LUID &adapter, IDXGIDevice *broken);

    void clear();

  private:
    static constexpr unsigned max_adapters = 8;

    struct entry_t {
      LUID luid;
      Microsoft::WRL::ComPtr<IDXGIDevice> device;
      unsigned users;
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
