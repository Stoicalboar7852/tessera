/**
 * @file tools/vdd-stress/src/probes.cpp
 * @brief Stall probes and CPU sampler for vdd-stress (see probes.h).
 */
// standard includes
#include <algorithm>
#include <iterator>
#include <map>
#include <utility>

// platform includes
#include <windows.h>
#include <dwmapi.h>
#include <timeapi.h>
#include <tlhelp32.h>

// local includes
#include "probes.h"
#include "tessera/win32.h"

namespace tessera::stress {

  namespace {

    constexpr const wchar_t *probe_window_class = L"TesseraVddStressProbe";

    std::uint64_t filetime_100ns(const FILETIME &ft) {
      return (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    }

    /// Bottom-right corner of the primary display, where the 4x4 compose-probe window sits.
    POINT probe_position() {
      return {GetSystemMetrics(SM_CXSCREEN) - 4, GetSystemMetrics(SM_CYSCREEN) - 4};
    }

  }  // namespace

  probes_t::probes_t(steady_t::time_point epoch, double record_ms):
      epoch_(epoch),
      record_ms_(record_ms) {}

  probes_t::~probes_t() {
    stop();
  }

  status_t probes_t::start() {
    if (running_.exchange(true)) {
      return status_t::ok();
    }
    timer_period_set_ = timeBeginPeriod(1) == TIMERR_NOERROR;
    threads_.emplace_back([this] {
      run_sched();
    });
    threads_.emplace_back([this] {
      run_compose();
    });
    threads_.emplace_back([this] {
      run_ui();
    });
    threads_.emplace_back([this] {
      run_cpu();
    });
    return status_t::ok();
  }

  void probes_t::stop() {
    if (!running_.exchange(false)) {
      return;
    }
    for (auto &t : threads_) {
      if (t.joinable()) {
        t.join();
      }
    }
    threads_.clear();
    if (timer_period_set_) {
      timeEndPeriod(1);
      timer_period_set_ = false;
    }
  }

  std::vector<gap_t> probes_t::gaps() const {
    std::lock_guard lock(mutex_);
    return gaps_;
  }

  std::vector<cpu_sample_t> probes_t::cpu() const {
    std::lock_guard lock(mutex_);
    return cpu_;
  }

  probe_stats_t probes_t::stats(const std::string &probe) const {
    std::lock_guard lock(mutex_);
    for (const auto &[name, s] : stats_) {
      if (name == probe) {
        return s;
      }
    }
    return {};
  }

  void probes_t::sample(const char *probe, bool ok, steady_t::time_point start, double duration_ms) {
    std::lock_guard lock(mutex_);
    auto it = std::find_if(stats_.begin(), stats_.end(), [&](const auto &p) {
      return p.first == probe;
    });
    if (it == stats_.end()) {
      stats_.emplace_back(probe, probe_stats_t {});
      it = std::prev(stats_.end());
    }
    ++it->second.samples;
    if (!ok) {
      ++it->second.errors;
    }
    it->second.max_ms = std::max(it->second.max_ms, duration_ms);
    if (duration_ms >= record_ms_) {
      gaps_.push_back({probe, ms_between(epoch_, start), duration_ms});
    }
  }

  void probes_t::run_sched() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    while (running_) {
      const auto t0 = steady_t::now();
      Sleep(1);
      sample("sched", true, t0, ms_between(t0, steady_t::now()));
    }
  }

  void probes_t::run_compose() {
    WNDCLASSEXW wc {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = probe_window_class;
    RegisterClassExW(&wc);  // fails harmlessly if already registered
    const POINT pos = probe_position();
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT, probe_window_class, L"", WS_POPUP, pos.x, pos.y, 4, 4, nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd == nullptr) {
      sample("compose", false);
      return;
    }
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    HBRUSH brushes[2] = {CreateSolidBrush(RGB(0, 0, 0)), CreateSolidBrush(RGB(32, 32, 32))};
    unsigned frame = 0;
    auto last = steady_t::now();
    auto last_move = last;
    while (running_) {
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        DispatchMessageW(&msg);
      }
      // Keep the window on the primary display, whose size may have just changed.
      if (ms_between(last_move, steady_t::now()) > 1000) {
        const POINT p = probe_position();
        SetWindowPos(hwnd, HWND_TOPMOST, p.x, p.y, 4, 4, SWP_NOACTIVATE | SWP_NOSIZE);
        last_move = steady_t::now();
      }
      if (HDC dc = GetDC(hwnd); dc != nullptr) {
        RECT r {0, 0, 4, 4};
        FillRect(dc, &r, brushes[++frame & 1]);
        ReleaseDC(hwnd, dc);
      }
      GdiFlush();
      const HRESULT hr = DwmFlush();  // returns after DWM has composed the pending update
      const auto now = steady_t::now();
      if (FAILED(hr)) {
        sample("compose", false);
        Sleep(5);
        continue;
      }
      sample("compose", true, last, ms_between(last, now));
      last = now;
    }
    DestroyWindow(hwnd);
    DeleteObject(brushes[0]);
    DeleteObject(brushes[1]);
  }

  void probes_t::run_ui() {
    while (running_) {
      HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
      if (tray == nullptr) {
        sample("ui", false);  // Explorer is not running (or restarting)
        Sleep(50);
        continue;
      }
      const auto t0 = steady_t::now();
      DWORD_PTR result = 0;
      const bool ok = SendMessageTimeoutW(tray, WM_NULL, 0, 0, SMTO_NORMAL, 10000, &result) != 0;
      sample("ui", ok, t0, ms_between(t0, steady_t::now()));
      Sleep(20);
    }
  }

  void probes_t::run_cpu() {
    constexpr std::size_t name_count = 3;
    static const wchar_t *const names[name_count] = {L"dwm.exe", L"WUDFHost.exe", L"explorer.exe"};
    struct last_t {
      std::uint64_t cpu_100ns = 0;
    };

    // Keyed by (pid, creation time) so a reused PID is not mistaken for the old process.
    std::map<std::pair<DWORD, std::uint64_t>, last_t> last;
    auto prev_wall = steady_t::now();
    while (running_) {
      for (int i = 0; i < 20 && running_; ++i) {
        Sleep(50);
      }
      const auto now = steady_t::now();
      const double wall_100ns = ms_between(prev_wall, now) * 10000.0;
      prev_wall = now;

      win32::unique_handle_t snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
      if (!snap) {
        sample("cpu", false);
        continue;
      }
      std::map<std::pair<DWORD, std::uint64_t>, last_t> seen;
      double totals[name_count] = {};
      std::uint32_t instances[name_count] = {};
      PROCESSENTRY32W pe {};
      pe.dwSize = sizeof(pe);
      for (BOOL more = Process32FirstW(snap.get(), &pe); more; more = Process32NextW(snap.get(), &pe)) {
        std::size_t idx = name_count;
        for (std::size_t n = 0; n < name_count; ++n) {
          if (_wcsicmp(pe.szExeFile, names[n]) == 0) {
            idx = n;
          }
        }
        if (idx == name_count) {
          continue;
        }
        win32::unique_handle_t proc(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID));
        FILETIME created, exited, kernel, user;
        if (!proc || !GetProcessTimes(proc.get(), &created, &exited, &kernel, &user)) {
          continue;
        }
        const auto key = std::make_pair(pe.th32ProcessID, filetime_100ns(created));
        const std::uint64_t cpu = filetime_100ns(kernel) + filetime_100ns(user);
        ++instances[idx];
        if (const auto it = last.find(key); it != last.end() && wall_100ns > 0) {
          totals[idx] += static_cast<double>(cpu - it->second.cpu_100ns) / wall_100ns * 100.0;
        }
        seen[key] = {cpu};
      }
      last = std::move(seen);
      {
        std::lock_guard lock(mutex_);
        for (std::size_t n = 0; n < name_count; ++n) {
          // Without elevation dwm.exe and WUDFHost.exe cannot be opened; leave them out rather than report 0 %.
          if (instances[n] != 0) {
            cpu_.push_back({ms_between(epoch_, now), win32::to_utf8(names[n]), totals[n], instances[n]});
          }
        }
      }
      sample("cpu", true);
    }
  }

}  // namespace tessera::stress
