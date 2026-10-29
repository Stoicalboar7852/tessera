/**
 * @file tools/vdd-stress/src/main.cpp
 * @brief vdd-stress.exe: adds and removes virtual displays in cycles and measures how long each change takes
 *        and whether the system stalls meanwhile (brief §4.5, Phase 1).
 *
 *   vdd-stress run   --backend B [--counts 1,2,4,8] [--cycles 10] [--mode 1920x1080@60] [--hold-ms 2000]
 *                    [--settle-ms 500] [--timeout-ms 10000] [--extend-after-ms 4000] [--record-ms 50]
 *                    [--out DIR] [--mtt-config PATH] [--batch] [--headless]
 *       --batch     add all displays of a cycle in one request (backends that support it: tessera)
 *       --headless  keep one extra virtual display and switch the physical displays off for the run (journaled,
 *                   so recover.exe can undo it), then switch them back on
 *       --hdr       add HDR10 monitors (tessera)
 *       --new-ids   a never-seen monitor for every add (tessera; default: one stable ID per slot, like a host)
 *   vdd-stress hold  --backend B --count N --seconds S [--mode ...] [--out DIR]   (steady state with N displays)
 *   vdd-stress probe --seconds S [--out DIR]                                     (baseline, no driver)
 *   vdd-stress save FILE | restore FILE                                          (display layout as JSON)
 *
 * Each cycle adds `count` displays one by one (or restarts a fixed-count driver with `count` displays), waits
 * until each is active in the CCD topology, holds, then removes them again. The display layout from before the
 * run is restored at the end. Output: events.csv, gaps.csv, cpu.csv and summary.json in --out.
 * The displays are kept on while it runs, and the summary records any time the session was locked, the console
 * display was off or the session was remote, since measurements taken then are not comparable.
 * Exit codes: 0 targets met, 1 targets missed or an operation failed, 2 usage, 3 aborted.
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// platform includes
#include <windows.h>
#include <objbase.h>
#include <wtsapi32.h>

// local includes
#include "backends.h"
#include "probes.h"
#include "tessera/display_topology.h"
#include "tessera/state_restore.h"
#include "tessera/win32.h"

namespace fs = std::filesystem;
using namespace tessera;
using namespace tessera::stress;
using nlohmann::json;

namespace {

  // Targets from brief §4.5.
  constexpr double target_change_ms = 3000;
  constexpr double target_stall_ms = 250;

  std::atomic<bool> g_abort {false};

  BOOL WINAPI on_console_ctrl(DWORD) {
    g_abort = true;
    return TRUE;
  }

  struct options_t {
    std::string command;
    std::string backend;
    std::vector<std::uint32_t> counts {1, 2, 4, 8};
    std::uint32_t cycles = 10;
    display_mode_t mode;
    std::uint32_t hold_ms = 2000;
    std::uint32_t settle_ms = 500;
    std::uint32_t timeout_ms = 10000;
    std::uint32_t extend_after_ms = 4000;
    double record_ms = 50;
    std::uint32_t seconds = 60;
    fs::path out = L"vdd-stress-out";
    backend_options_t backend_options;
    bool batch = false;
    bool headless = false;
  };

  struct event_t {
    double t_ms = 0;
    std::string op;  ///< add, remove, set_count
    std::uint32_t count = 0;  ///< Displays in this cycle.
    std::uint32_t cycle = 0;
    std::uint32_t index = 0;  ///< Display number within the cycle (add/remove), or the new count (set_count).
    double call_ms = 0;  ///< Time inside the driver call.
    double visible_ms = -1;  ///< From the call until the CCD topology showed the change; -1 = never.
    double reapply_ms = -1;  ///< --headless: switching the physical displays off again after Windows switched them on.
    double end_ms = 0;
    bool ok = false;
    bool away = false;  ///< Session locked, console display off or session remote during the operation.
    std::string detail;
  };

  void usage() {
    std::puts(
      "Usage:\n"
      "  vdd-stress run   --backend tessera|sudovda|parsec|mtt [--counts 1,2,4,8] [--cycles 10]\n"
      "                   [--mode 1920x1080@60] [--hold-ms 2000] [--settle-ms 500] [--timeout-ms 10000]\n"
      "                   [--extend-after-ms 4000] [--record-ms 50] [--out DIR] [--mtt-config PATH]\n"
      "                   [--batch] [--headless] [--hdr] [--new-ids]\n"
      "  vdd-stress hold  --backend B --count N --seconds S [--mode M] [--out DIR]\n"
      "  vdd-stress probe --seconds S [--out DIR]\n"
      "  vdd-stress save FILE | restore FILE\n"
      "Needs an elevated prompt for run/hold (driver control interfaces are admin-only)."
    );
  }

  result_t<options_t> parse(const std::vector<std::string> &args) {
    options_t o;
    if (args.empty()) {
      return make_error(tessera_code_e::invalid_data, "args", "missing command");
    }
    o.command = args[0];
    auto number = [](const std::string &s) -> std::uint32_t {
      return static_cast<std::uint32_t>(std::stoul(s));
    };
    try {
      for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string &a = args[i];
        if (a == "--batch" || a == "--headless" || a == "--hdr" || a == "--new-ids") {
          (a == "--batch" ? o.batch : a == "--headless" ? o.headless : a == "--hdr" ? o.backend_options.hdr : o.backend_options.new_ids) = true;
          continue;
        }
        if (i + 1 >= args.size()) {
          return make_error(tessera_code_e::invalid_data, "args", "missing value for " + a);
        }
        const std::string &v = args[++i];
        if (a == "--backend") {
          o.backend = v;
        } else if (a == "--counts") {
          o.counts.clear();
          std::stringstream ss(v);
          for (std::string item; std::getline(ss, item, ',');) {
            o.counts.push_back(number(item));
          }
        } else if (a == "--count") {
          o.counts = {number(v)};
        } else if (a == "--cycles") {
          o.cycles = number(v);
        } else if (a == "--mode") {
          auto m = parse_mode(v);
          if (!m) {
            return m.error();
          }
          o.mode = m.value();
        } else if (a == "--hold-ms") {
          o.hold_ms = number(v);
        } else if (a == "--settle-ms") {
          o.settle_ms = number(v);
        } else if (a == "--timeout-ms") {
          o.timeout_ms = number(v);
        } else if (a == "--extend-after-ms") {
          o.extend_after_ms = number(v);
        } else if (a == "--record-ms") {
          o.record_ms = std::stod(v);
        } else if (a == "--seconds") {
          o.seconds = number(v);
        } else if (a == "--out") {
          o.out = fs::path(win32::to_wide(v));
        } else if (a == "--mtt-config") {
          o.backend_options.mtt_config = v;
        } else {
          return make_error(tessera_code_e::invalid_data, "args", "unknown option " + a);
        }
      }
    } catch (const std::exception &) {
      return make_error(tessera_code_e::invalid_data, "args", "invalid number");
    }
    if (o.command == "hold") {
      o.cycles = 1;
      o.hold_ms = o.seconds * 1000;
    }
    const bool needs_backend = o.command == "run" || o.command == "hold";
    if (needs_backend && o.backend.empty()) {
      return make_error(tessera_code_e::invalid_data, "args", "--backend is required");
    }
    if (o.counts.empty() || std::any_of(o.counts.begin(), o.counts.end(), [](auto n) {
          return n == 0 || n > 16;
        })) {
      return make_error(tessera_code_e::invalid_data, "args", "counts must be 1..16");
    }
    return o;
  }

  /// Sleeps in small steps so Ctrl+C is honoured.
  void nap(std::uint32_t ms) {
    const auto end = steady_t::now() + std::chrono::milliseconds(ms);
    while (!g_abort && steady_t::now() < end) {
      Sleep(10);
    }
  }

  /**
   * @brief Waits until the number of active display paths equals `wanted`.
   * @return Milliseconds since `since`, or -1 on timeout. Asks Windows to extend the desktop once if a new
   *         display stays inactive for `extend_after_ms` (Windows normally extends on its own).
   */
  double wait_active(std::size_t wanted, steady_t::time_point since, const options_t &o, std::string &detail);

  /// Active display targets (CCD), or nullopt if the query failed.
  std::optional<std::vector<target_ref_t>> active_targets() {
    for (int attempt = 0; attempt < 5; ++attempt) {
      UINT32 paths = 0;
      UINT32 modes = 0;
      if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS) {
        return std::nullopt;
      }
      std::vector<DISPLAYCONFIG_PATH_INFO> p(paths);
      std::vector<DISPLAYCONFIG_MODE_INFO> m(modes);
      const LONG rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, p.data(), &modes, m.data(), nullptr);
      if (rc == ERROR_INSUFFICIENT_BUFFER) {
        continue;  // the topology changed between the two calls
      }
      if (rc != ERROR_SUCCESS) {
        return std::nullopt;
      }
      std::vector<target_ref_t> out;
      for (UINT32 i = 0; i < paths; ++i) {
        out.push_back({p[i].targetInfo.adapterId.LowPart, p[i].targetInfo.adapterId.HighPart, p[i].targetInfo.id});
      }
      return out;
    }
    return std::nullopt;
  }

  /// GUID_CONSOLE_DISPLAY_STATE, defined here so that no import library has to provide it.
  constexpr GUID console_display_state = {0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};

  /// How long a condition (session locked, display off, ...) held during the run.
  class condition_t {
  public:
    void set(bool on) {
      std::lock_guard lock(mutex_);
      const auto now = steady_t::now();
      if (on && !on_) {
        since_ = now;
        ++count_;
      } else if (!on && on_) {
        total_ms_ += ms_between(since_, now);
      }
      on_ = on;
    }

    bool on() const {
      std::lock_guard lock(mutex_);
      return on_;
    }

    json to_json() const {
      std::lock_guard lock(mutex_);
      const double ms = total_ms_ + (on_ ? ms_between(since_, steady_t::now()) : 0.0);
      return {{"ms", ms}, {"times", count_}, {"at_end", on_}};
    }

  private:
    mutable std::mutex mutex_;
    bool on_ = false;
    steady_t::time_point since_;
    double total_ms_ = 0;
    unsigned count_ = 0;
  };

  /**
   * @brief Signals display configuration changes (WM_DISPLAYCHANGE, broadcast to every top-level window, also hidden
   *        ones), so waits re-check the topology when it changes instead of polling the CCD API, whose calls compete
   *        with Windows' own handling of the change.
   *
   * Also records when the session is locked, the console display is off (idle timeout) or the session is remote:
   * Windows and the shell then handle display changes differently, so such measurements are not comparable.
   */
  class display_watch_t {
  public:
    display_watch_t() {
      changed_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      thread_ = std::thread([this] {
        run();
      });
      WaitForSingleObject(ready_, 5000);
    }

    ~display_watch_t() {
      if (HWND h = hwnd_.load(); h != nullptr) {
        PostMessageW(h, WM_CLOSE, 0, 0);
      }
      if (thread_.joinable()) {
        thread_.join();
      }
      CloseHandle(changed_);
      CloseHandle(ready_);
    }

    display_watch_t(const display_watch_t &) = delete;
    display_watch_t &operator=(const display_watch_t &) = delete;

    /// Returns when the display configuration changed or after `ms`, whichever comes first.
    void wait(DWORD ms) const {
      WaitForSingleObject(changed_, ms);
    }

    /// The session is locked, the console display is off or the session is remote.
    bool away() const {
      return locked_.on() || display_off_.on() || remote_.on();
    }

    json environment() const {
      return {{"session_locked", locked_.to_json()}, {"display_off", display_off_.to_json()}, {"remote_session", remote_.to_json()}};
    }

  private:
    static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
      auto *self = reinterpret_cast<display_watch_t *>(GetWindowLongPtrW(h, GWLP_USERDATA));
      if (msg == WM_DISPLAYCHANGE) {
        if (self != nullptr) {
          SetEvent(self->changed_);
        }
      } else if (msg == WM_WTSSESSION_CHANGE && self != nullptr) {
        if (wp == WTS_SESSION_LOCK || wp == WTS_SESSION_UNLOCK) {
          self->locked_.set(wp == WTS_SESSION_LOCK);
        } else if (wp == WTS_REMOTE_CONNECT || wp == WTS_REMOTE_DISCONNECT) {
          self->remote_.set(wp == WTS_REMOTE_CONNECT);
        }
      } else if (msg == WM_POWERBROADCAST && wp == PBT_POWERSETTINGCHANGE && self != nullptr) {
        const auto *s = reinterpret_cast<const POWERBROADCAST_SETTING *>(lp);
        if (s != nullptr && IsEqualGUID(s->PowerSetting, console_display_state) && s->DataLength >= sizeof(DWORD)) {
          DWORD state = 0;  // 0 off, 1 on, 2 dimmed
          std::memcpy(&state, s->Data, sizeof(state));
          self->display_off_.set(state == 0);
        }
        return TRUE;
      } else if (msg == WM_CLOSE) {
        DestroyWindow(h);
        return 0;
      } else if (msg == WM_DESTROY) {
        WTSUnRegisterSessionNotification(h);
        PostQuitMessage(0);
        return 0;
      }
      return DefWindowProcW(h, msg, wp, lp);
    }

    void run() {
      WNDCLASSEXW wc {};
      wc.cbSize = sizeof(wc);
      wc.lpfnWndProc = wndproc;
      wc.hInstance = GetModuleHandleW(nullptr);
      wc.lpszClassName = L"TesseraVddStressWatch";
      RegisterClassExW(&wc);
      HWND h = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
      HPOWERNOTIFY display_state = nullptr;
      if (h != nullptr) {
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        hwnd_ = h;
        locked_.set(session_locked());
        remote_.set(GetSystemMetrics(SM_REMOTESESSION) != 0);
        WTSRegisterSessionNotification(h, NOTIFY_FOR_THIS_SESSION);
        // Sends the current state right away, then every change.
        display_state = RegisterPowerSettingNotification(h, &console_display_state, DEVICE_NOTIFY_WINDOW_HANDLE);
      }
      SetEvent(ready_);
      if (h == nullptr) {
        return;
      }
      MSG msg;
      while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        DispatchMessageW(&msg);
      }
      if (display_state != nullptr) {
        UnregisterPowerSettingNotification(display_state);
      }
    }

    static bool session_locked() {
      WTSINFOEXW *info = nullptr;
      DWORD size = 0;
      bool locked = false;
      if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSSessionInfoEx, reinterpret_cast<LPWSTR *>(&info), &size) && info != nullptr) {
        locked = info->Level == 1 && info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK;
        WTSFreeMemory(info);
      }
      return locked;
    }

    condition_t locked_;
    condition_t display_off_;
    condition_t remote_;
    HANDLE changed_ = nullptr;
    HANDLE ready_ = nullptr;
    std::atomic<HWND> hwnd_ {nullptr};
    std::thread thread_;
  };

  const display_watch_t *g_watch = nullptr;

  /// Re-checks at least this often even without a WM_DISPLAYCHANGE (it is not sent for every path change).
  constexpr DWORD recheck_ms = 100;

  void wait_for_change() {
    if (g_watch != nullptr) {
      g_watch->wait(recheck_ms);
    } else {
      Sleep(recheck_ms);
    }
  }

  void add_detail(std::string &detail, const std::string &text) {
    detail += (detail.empty() ? "" : "; ") + text;
  }

  bool contains(const std::vector<target_ref_t> &v, const target_ref_t &t) {
    return std::find(v.begin(), v.end(), t) != v.end();
  }

  double wait_active(std::size_t wanted, steady_t::time_point since, const options_t &o, std::string &detail) {
    bool extended = false;
    while (!g_abort) {
      const auto now = active_targets();
      const double elapsed = ms_between(since, steady_t::now());
      if (now && now->size() == wanted) {
        return elapsed;
      }
      if (elapsed > o.timeout_ms) {
        add_detail(detail, "timeout with " + (now ? std::to_string(now->size()) : std::string("?")) + " active paths, wanted " + std::to_string(wanted));
        return -1;
      }
      if (!extended && now && now->size() < wanted && elapsed > o.extend_after_ms) {
        extended = true;
        const LONG rc = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_EXTEND);
        add_detail(detail, "asked Windows to extend (rc " + std::to_string(rc) + ")");
      }
      wait_for_change();
    }
    add_detail(detail, "aborted");
    return -1;
  }

  /// Waits until every target in `active` is active and none in `gone` is. -1 on timeout.
  double wait_targets(const std::vector<target_ref_t> &active, const std::vector<target_ref_t> &gone, steady_t::time_point since, const options_t &o, std::string &detail) {
    bool extended = false;
    while (!g_abort) {
      const auto now = active_targets();
      const double elapsed = ms_between(since, steady_t::now());
      std::size_t missing = 0;
      std::size_t lingering = 0;
      if (now) {
        for (const auto &t : active) {
          missing += contains(*now, t) ? 0 : 1;
        }
        for (const auto &t : gone) {
          lingering += contains(*now, t) ? 1 : 0;
        }
        if (missing == 0 && lingering == 0) {
          return elapsed;
        }
      }
      if (elapsed > o.timeout_ms) {
        add_detail(detail, "timeout: " + std::to_string(missing) + " display(s) not active, " + std::to_string(lingering) + " still active");
        return -1;
      }
      if (!extended && missing != 0 && elapsed > o.extend_after_ms) {
        extended = true;
        const LONG rc = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_EXTEND);
        add_detail(detail, "asked Windows to extend (rc " + std::to_string(rc) + ")");
      }
      wait_for_change();
    }
    add_detail(detail, "aborted");
    return -1;
  }

  /// Runs keepalive() on a timer while it is alive.
  class keepalive_t {
  public:
    keepalive_t(backend_t &backend, std::vector<std::string> &errors):
        backend_(backend),
        errors_(errors) {
      const auto interval = backend_.keepalive_interval();
      if (interval.count() == 0) {
        return;
      }
      thread_ = std::thread([this, interval] {
        while (running_) {
          if (auto st = backend_.keepalive(); !st) {
            std::lock_guard lock(mutex_);
            if (errors_.size() < 20) {
              errors_.push_back(st.error().to_string());
            }
          }
          const auto end = steady_t::now() + interval;
          while (running_ && steady_t::now() < end) {
            Sleep(5);
          }
        }
      });
    }

    ~keepalive_t() {
      running_ = false;
      if (thread_.joinable()) {
        thread_.join();
      }
    }

  private:
    backend_t &backend_;
    std::vector<std::string> &errors_;
    std::mutex mutex_;
    std::atomic<bool> running_ {true};
    std::thread thread_;
  };

  double percentile(std::vector<double> v, double p) {
    if (v.empty()) {
      return 0;
    }
    std::sort(v.begin(), v.end());
    const auto idx = static_cast<std::size_t>(p / 100.0 * static_cast<double>(v.size() - 1) + 0.5);
    return v[std::min(idx, v.size() - 1)];
  }

  json distribution(const std::vector<double> &v) {
    if (v.empty()) {
      return json {{"n", 0}};
    }
    return json {{"n", v.size()}, {"median", percentile(v, 50)}, {"p95", percentile(v, 95)}, {"max", *std::max_element(v.begin(), v.end())}};
  }

  /// The operation a gap overlaps, or "hold".
  std::string gap_context(const gap_t &g, const std::vector<event_t> &events) {
    for (const auto &e : events) {
      if (g.t_ms < e.end_ms && g.t_ms + g.duration_ms > e.t_ms) {
        return e.op + "#" + std::to_string(e.count) + "." + std::to_string(e.cycle) + "." + std::to_string(e.index);
      }
    }
    return "hold";
  }

  void write_csv(const fs::path &dir, const std::vector<event_t> &events, const probes_t &probes) {
    std::ofstream ev(dir / L"events.csv", std::ios::trunc);
    ev << "t_ms,op,count,cycle,index,call_ms,visible_ms,reapply_ms,ok,away,detail\n";
    for (const auto &e : events) {
      std::string detail = e.detail;
      std::replace(detail.begin(), detail.end(), ',', ';');
      ev << e.t_ms << ',' << e.op << ',' << e.count << ',' << e.cycle << ',' << e.index << ',' << e.call_ms << ',' << e.visible_ms << ',' << e.reapply_ms << ',' << (e.ok ? 1 : 0) << ',' << (e.away ? 1 : 0) << ',' << detail << '\n';
    }
    std::ofstream gp(dir / L"gaps.csv", std::ios::trunc);
    gp << "probe,t_ms,duration_ms,during\n";
    for (const auto &g : probes.gaps()) {
      gp << g.probe << ',' << g.t_ms << ',' << g.duration_ms << ',' << gap_context(g, events) << '\n';
    }
    std::ofstream cp(dir / L"cpu.csv", std::ios::trunc);
    cp << "t_ms,process,cpu_percent,instances\n";
    for (const auto &c : probes.cpu()) {
      cp << c.t_ms << ',' << c.process << ',' << c.cpu_percent << ',' << c.instances << '\n';
    }
  }

  json summarize(const options_t &o, const backend_t *backend, const std::vector<event_t> &events, const probes_t &probes, const std::vector<std::string> &errors, bool aborted) {
    json s;
    s["command"] = o.command;
    s["backend"] = backend ? backend->name() : "none";
    if (backend && !backend->notes().empty()) {
      s["backend_notes"] = backend->notes();
    }
    s["mode"] = o.mode.to_string();
    s["cycles"] = o.cycles;
    s["hold_ms"] = o.hold_ms;
    s["aborted"] = aborted;
    s["batch"] = o.batch;
    s["headless"] = o.headless;
    s["hdr"] = o.backend_options.hdr;
    if (backend != nullptr && std::string(backend->name()) == "tessera") {
      s["monitor_ids"] = o.backend_options.new_ids ? "new" : "stable";
    }
    std::vector<double> reapply;
    std::size_t away = 0;
    for (const auto &e : events) {
      if (e.reapply_ms >= 0) {
        reapply.push_back(e.reapply_ms);
      }
      away += e.away ? 1 : 0;
    }
    s["ops_while_away"] = away;
    if (o.headless) {
      s["headless_reapply_ms"] = distribution(reapply);
    }

    bool targets_met = !aborted;
    json ops = json::array();
    std::vector<std::string> kinds;
    for (const auto &e : events) {
      if (std::find(kinds.begin(), kinds.end(), e.op) == kinds.end()) {
        kinds.push_back(e.op);
      }
    }
    for (const auto &kind : kinds) {
      for (const auto n : o.counts) {
        std::vector<double> call, visible;
        std::size_t failed = 0;
        std::size_t timeouts = 0;
        for (const auto &e : events) {
          if (e.op != kind || e.count != n) {
            continue;
          }
          call.push_back(e.call_ms);
          if (e.visible_ms >= 0) {
            visible.push_back(e.visible_ms);
          } else {
            ++timeouts;
          }
          failed += e.ok ? 0 : 1;
        }
        if (call.empty()) {
          continue;
        }
        const double worst = visible.empty() ? 0 : *std::max_element(visible.begin(), visible.end());
        const bool met = failed == 0 && timeouts == 0 && worst < target_change_ms;
        targets_met = targets_met && met;
        ops.push_back({{"op", kind}, {"count", n}, {"failed", failed}, {"timeouts", timeouts}, {"call_ms", distribution(call)}, {"visible_ms", distribution(visible)}, {"under_3s", met}});
      }
    }
    s["operations"] = ops;

    const auto gaps = probes.gaps();
    json probe_summary = json::object();
    for (const char *name : {"compose", "ui", "sched"}) {
      std::vector<double> d;
      std::size_t over = 0;
      std::size_t over_during_ops = 0;
      json worst = json::array();
      std::vector<gap_t> mine;
      for (const auto &g : gaps) {
        if (g.probe == name) {
          mine.push_back(g);
        }
      }
      std::sort(mine.begin(), mine.end(), [](const gap_t &a, const gap_t &b) {
        return a.duration_ms > b.duration_ms;
      });
      for (const auto &g : mine) {
        d.push_back(g.duration_ms);
        if (g.duration_ms > target_stall_ms) {
          ++over;
          over_during_ops += gap_context(g, events) == "hold" ? 0 : 1;
        }
        if (worst.size() < 10) {
          worst.push_back({{"t_ms", g.t_ms}, {"duration_ms", g.duration_ms}, {"during", gap_context(g, events)}});
        }
      }
      const auto st = probes.stats(name);
      probe_summary[name] = {{"samples", st.samples}, {"errors", st.errors}, {"recorded_gaps", d.size()}, {"max_ms", st.max_ms}, {"over_250ms", over}, {"over_250ms_during_ops", over_during_ops}, {"worst", worst}};
      targets_met = targets_met && over == 0;
    }
    s["probes"] = probe_summary;
    s["record_ms"] = o.record_ms;

    // Mean CPU per process while the run was going.
    json cpu = json::object();
    for (const auto &c : probes.cpu()) {
      auto &entry = cpu[c.process];
      if (entry.is_null()) {
        entry = {{"sum", 0.0}, {"n", 0}, {"max", 0.0}};
      }
      entry["sum"] = entry.value("sum", 0.0) + c.cpu_percent;
      entry["n"] = entry.value("n", 0) + 1;
      entry["max"] = std::max(entry.value("max", 0.0), c.cpu_percent);
    }
    for (auto &[name, entry] : cpu.items()) {
      entry["mean"] = entry["n"].get<int>() ? entry["sum"].get<double>() / entry["n"].get<int>() : 0.0;
      entry.erase("sum");
    }
    s["cpu_percent"] = cpu;
    s["keepalive_errors"] = errors;
    s["targets_met"] = targets_met;
    s["targets"] = {{"change_ms", target_change_ms}, {"stall_ms", target_stall_ms}};
    return s;
  }

  /// Removes whatever is still added and puts the original layout back.
  void cleanup(backend_t &b, std::vector<std::uint32_t> &handles, bool count_active, const display::topology_t &before, std::size_t base, const options_t &o) {
    const bool was_aborted = g_abort.exchange(false);  // cleanup must run even after Ctrl+C
    if (b.dynamic()) {
      while (!handles.empty()) {
        if (auto st = b.remove(handles.back()); !st) {
          std::fprintf(stderr, "cleanup: %s\n", st.error().to_string().c_str());
        }
        handles.pop_back();
      }
    } else if (count_active) {
      if (auto st = b.set_count(0, o.mode); !st) {
        std::fprintf(stderr, "cleanup: %s\n", st.error().to_string().c_str());
      }
    }
    std::string detail;
    wait_active(base, steady_t::now(), o, detail);
    auto now = display::capture();
    if (now && !display::compare(before, now.value()).empty()) {
      std::puts("restoring the display layout from before the run");
      if (auto st = display::apply(before); !st) {
        std::fprintf(stderr, "restore failed: %s (run recover.exe or topoctl)\n", st.error().to_string().c_str());
      }
    }
    g_abort = was_aborted;
  }

  /// --headless: one extra virtual display stays active while the physical displays are switched off.
  struct headless_t {
    bool anchored = false;
    std::uint32_t anchor = 0;
    display::path_t anchor_path;  ///< The anchor as the CCD API names it; stays the primary.
    display::topology_t restore_to;  ///< Physical displays plus the anchor, as before switching off.
    std::unique_ptr<state::state_journal_t> journal;
    std::string entry;  ///< Journal entry of the change; recover.exe undoes it if this process dies.
  };

  bool same_target(const display::path_t &a, const display::path_t &b) {
    return a.adapter_path == b.adapter_path && a.target_id == b.target_id;
  }

  /// The active paths of `now` that are not physical displays (not active `before` the run), moved so that
  /// `origin` is the primary.
  result_t<display::topology_t> virtual_only(const display::topology_t &before, const display::topology_t &now, const display::path_t &origin) {
    display::topology_t wanted;
    for (const auto &p : now.paths) {
      if (std::none_of(before.paths.begin(), before.paths.end(), [&](const display::path_t &q) {
            return same_target(p, q);
          })) {
        wanted.paths.push_back(p);
      }
    }
    const auto o = std::find_if(wanted.paths.begin(), wanted.paths.end(), [&](const display::path_t &p) {
      return same_target(p, origin);
    });
    if (o == wanted.paths.end()) {
      return make_error(tessera_code_e::unsafe, "headless", "the anchor display is not active");
    }
    const std::int32_t dx = o->position_x;
    const std::int32_t dy = o->position_y;
    for (auto &p : wanted.paths) {
      p.position_x -= dx;
      p.position_y -= dy;
    }
    return wanted;
  }

  /**
   * @brief Switches the physical displays off again after Windows switched them back on, as a headless host must:
   *        Windows applies its saved (or default "extend") layout for each new set of monitors.
   */
  status_t reapply_headless(const display::topology_t &before, const headless_t &h) {
    auto now = display::capture();
    if (!now) {
      return now.error();
    }
    auto wanted = virtual_only(before, now.value(), h.anchor_path);
    if (!wanted) {
      return wanted.error();
    }
    return display::apply(wanted.value());
  }

  status_t enter_headless(backend_t &b, const options_t &o, const display::topology_t &before, headless_t &h, std::size_t &base) {
    auto a = b.add(o.mode);
    if (!a) {
      return a.error();
    }
    h.anchored = true;
    h.anchor = a.value();
    std::string detail;
    if (wait_active(before.paths.size() + 1, steady_t::now(), o, detail) < 0) {
      return make_error(tessera_code_e::timeout, "headless", "the anchor display did not become active: " + detail);
    }
    auto now = display::capture();
    if (!now) {
      return now.error();
    }
    h.restore_to = now.value();
    const auto anchor = std::find_if(now.value().paths.begin(), now.value().paths.end(), [&](const display::path_t &p) {
      return std::none_of(before.paths.begin(), before.paths.end(), [&](const display::path_t &q) {
        return same_target(p, q);
      });
    });
    if (anchor == now.value().paths.end()) {
      return make_error(tessera_code_e::unsafe, "headless", "no virtual display to keep active");
    }
    h.anchor_path = *anchor;
    // The kept display becomes the primary (desktop origin).
    auto kept = virtual_only(before, now.value(), h.anchor_path);
    if (!kept) {
      return kept.error();
    }
    const display::topology_t &wanted = kept.value();
    auto path = state::state_journal_t::default_path();
    if (!path) {
      return path.error();
    }
    state::state_journal_t::options_t jo;
    jo.file = path.value();
    h.journal = std::make_unique<state::state_journal_t>(jo);
    auto id = state::journaled_display_change(*h.journal, "vdd-stress --headless", wanted);
    if (!id) {
      return id.error();
    }
    h.entry = id.value();
    base = wanted.paths.size();
    std::printf("headless: physical displays off, %zu virtual display(s) active (journal entry %s)\n", base, h.entry.c_str());
    return status_t::ok();
  }

  void leave_headless(backend_t &b, const options_t &o, headless_t &h, std::size_t physical) {
    const bool was_aborted = g_abort.exchange(false);
    if (!h.entry.empty()) {
      if (auto st = display::apply(h.restore_to); !st) {
        std::fprintf(stderr, "headless: restoring the physical displays failed: %s (the journal entry stays; run recover.exe)\n", st.error().to_string().c_str());
      } else if (auto rm = h.journal->remove(h.entry); !rm) {
        std::fprintf(stderr, "headless: %s\n", rm.error().to_string().c_str());
      }
    }
    if (h.anchored) {
      if (auto st = b.remove(h.anchor); !st) {
        std::fprintf(stderr, "headless: removing the anchor display: %s\n", st.error().to_string().c_str());
      }
      std::string detail;
      wait_active(physical, steady_t::now(), o, detail);
    }
    g_abort = was_aborted;
  }

  int run(const options_t &o) {
    auto made = make_backend(o.backend, o.backend_options);
    if (!made) {
      std::fprintf(stderr, "ERROR: %s\n", made.error().to_string().c_str());
      return 2;
    }
    auto &backend = *made.value();
    if (auto st = backend.open(); !st) {
      std::fprintf(stderr, "ERROR: %s\n", st.error().to_string().c_str());
      return 1;
    }
    auto before = display::capture();
    if (!before) {
      std::fprintf(stderr, "ERROR: %s\n", before.error().to_string().c_str());
      return 1;
    }
    const std::size_t physical = before.value().paths.size();
    std::size_t base = physical;
    if (base == 0) {
      std::fputs("ERROR: no active display; refusing to start\n", stderr);
      return 1;
    }
    if (o.headless && !backend.dynamic()) {
      std::fputs("ERROR: --headless needs a backend that adds displays at runtime\n", stderr);
      return 2;
    }
    if (o.batch && !backend.supports_batch()) {
      std::fprintf(stderr, "ERROR: --batch is not supported by %s\n", backend.name());
      return 2;
    }
    std::printf("%s: %zu active display(s) before the run; mode %s%s%s\n", backend.name(), base, o.mode.to_string().c_str(), o.batch ? ", batched" : "", o.headless ? ", headless" : "");
    if (!backend.notes().empty()) {
      std::printf("note: %s\n", backend.notes().c_str());
    }

    const auto epoch = steady_t::now();
    probes_t probes(epoch, o.record_ms);
    probes.start();
    std::vector<std::string> keepalive_errors;
    std::vector<event_t> events;
    std::vector<std::uint32_t> handles;
    bool count_active = false;
    headless_t headless;
    bool headless_failed = false;
    std::uint32_t physical_reactivations = 0;
    const std::vector<target_ref_t> physical_targets = active_targets().value_or(std::vector<target_ref_t> {});
    display_watch_t watch;
    g_watch = &watch;
    {
      keepalive_t keepalive(backend, keepalive_errors);
      if (o.headless) {
        if (auto st = enter_headless(backend, o, before.value(), headless, base); !st) {
          std::fprintf(stderr, "ERROR: %s\n", st.error().to_string().c_str());
          headless_failed = true;
          g_abort = true;
        }
      }
      // Waits for the change an operation made: by target when the backend reports targets (exact even when Windows
      // switches other displays on or off meanwhile), otherwise by the number of active paths.
      using waiter_t = std::function<double(steady_t::time_point, std::string &)>;
      auto by_count = [&](std::size_t wanted) -> waiter_t {
        return [&o, wanted](steady_t::time_point t0, std::string &detail) {
          return wait_active(wanted, t0, o, detail);
        };
      };
      auto targets_of = [&](const std::vector<std::uint32_t> &hs, bool &all_known) {
        std::vector<target_ref_t> out;
        all_known = !hs.empty();
        for (const auto h : hs) {
          const auto t = backend.target_of(h);
          if (t) {
            out.push_back(*t);
          } else {
            all_known = false;
          }
        }
        return out;
      };

      auto timed = [&](const std::string &op, std::uint32_t n, std::uint32_t cycle, std::uint32_t index, auto &&call, auto &&make_waiter) {
        event_t e;
        e.t_ms = ms_between(epoch, steady_t::now());
        e.op = op;
        e.count = n;
        e.cycle = cycle;
        e.index = index;
        e.away = watch.away();
        const auto t0 = steady_t::now();
        const status_t st = call();
        e.call_ms = ms_between(t0, steady_t::now());
        if (st) {
          const waiter_t wait = make_waiter();
          e.visible_ms = wait(t0, e.detail);
          e.ok = e.visible_ms >= 0;
        } else {
          e.detail = st.error().to_string();
        }
        if (o.headless && !physical_targets.empty() && !headless.entry.empty()) {
          // Windows applies its stored (or default "extend") topology for each new set of monitors, which can switch
          // the physical displays back on; a headless host must re-apply its own topology after each change.
          const auto now = active_targets();
          if (now && std::any_of(physical_targets.begin(), physical_targets.end(), [&](const target_ref_t &t) {
                return contains(*now, t);
              })) {
            ++physical_reactivations;
            add_detail(e.detail, "Windows switched physical displays back on");
            const auto r0 = steady_t::now();
            if (auto off = reapply_headless(before.value(), headless); !off) {
              add_detail(e.detail, "switching them off again failed: " + off.error().to_string());
            } else {
              std::string detail;
              e.reapply_ms = wait_targets({}, physical_targets, r0, o, detail);
              if (e.reapply_ms < 0) {
                add_detail(e.detail, "switching them off again: " + detail);
              }
            }
          }
        }
        e.away = e.away || watch.away();
        e.end_ms = ms_between(epoch, steady_t::now());
        events.push_back(e);
        if (!e.ok) {
          std::printf("  %s #%u FAILED: %s\n", op.c_str(), index, e.detail.c_str());
        }
        return e.ok;
      };

      for (const auto n : o.counts) {
        for (std::uint32_t c = 1; c <= o.cycles && !g_abort; ++c) {
          bool ok = true;
          if (o.batch) {
            ok = timed("add_batch", n, c, n, [&] {
              return backend.add_batch(o.mode, n, handles);
            }, [&]() -> waiter_t {
              bool known = false;
              const auto t = targets_of(handles, known);
              if (known) {
                return [&o, t](steady_t::time_point t0, std::string &detail) {
                  return wait_targets(t, {}, t0, o, detail);
                };
              }
              return by_count(base + n);
            });
            if (ok) {
              nap(o.hold_ms);
            }
            if (!handles.empty()) {
              const std::vector<std::uint32_t> all = handles;
              bool known = false;
              const auto t = targets_of(all, known);
              handles.clear();
              ok = timed("remove_batch", n, c, n, [&] {
                return backend.remove_batch(all);
              }, [&]() -> waiter_t {
                if (known) {
                  return [&o, t](steady_t::time_point t0, std::string &detail) {
                    return wait_targets({}, t, t0, o, detail);
                  };
                }
                return by_count(base);
              }) && ok;
            }
          } else if (backend.dynamic()) {
            for (std::uint32_t i = 1; i <= n && ok && !g_abort; ++i) {
              ok = timed("add", n, c, i, [&]() -> status_t {
                auto h = backend.add(o.mode);
                if (!h) {
                  return h.error();
                }
                handles.push_back(h.value());
                return status_t::ok();
              }, [&, i]() -> waiter_t {
                if (const auto t = backend.target_of(handles.back()); t) {
                  return [&o, t](steady_t::time_point t0, std::string &detail) {
                    return wait_targets({*t}, {}, t0, o, detail);
                  };
                }
                return by_count(base + i);
              });
            }
            if (ok) {
              nap(o.hold_ms);
            }
            for (std::uint32_t i = static_cast<std::uint32_t>(handles.size()); i >= 1 && !g_abort; --i) {
              const std::uint32_t h = handles.back();
              const auto t = backend.target_of(h);
              handles.pop_back();
              ok = timed("remove", n, c, i, [&] {
                return backend.remove(h);
              }, [&, i]() -> waiter_t {
                if (t) {
                  return [&o, t](steady_t::time_point t0, std::string &detail) {
                    return wait_targets({}, {*t}, t0, o, detail);
                  };
                }
                return by_count(base + i - 1);
              }) && ok;
            }
          } else {
            count_active = true;
            ok = timed("set_count", n, c, n, [&] {
              return backend.set_count(n, o.mode);
            }, [&] {
              return by_count(base + n);
            });
            nap(o.hold_ms);
            ok = timed("set_count", n, c, 0, [&] {
              return backend.set_count(0, o.mode);
            }, [&] {
              return by_count(base);
            }) && ok;
            count_active = !ok;
          }
          std::printf("count %u cycle %u/%u %s\n", n, c, o.cycles, ok ? "ok" : "FAILED");
          std::fflush(stdout);
          nap(o.settle_ms);
        }
      }
      if (o.headless) {
        leave_headless(backend, o, headless, physical);
      }
      cleanup(backend, handles, count_active, before.value(), physical, o);
    }
    probes.stop();
    g_watch = nullptr;
    if (headless_failed) {
      return 1;
    }

    std::error_code ec;
    fs::create_directories(o.out, ec);
    write_csv(o.out, events, probes);
    json s = summarize(o, &backend, events, probes, keepalive_errors, g_abort);
    s["environment"] = watch.environment();
    if (o.headless) {
      s["physical_reactivations"] = physical_reactivations;
    }
    std::ofstream(o.out / L"summary.json", std::ios::trunc) << s.dump(2) << '\n';
    std::printf("%s\n", s.dump(2).c_str());
    if (g_abort) {
      return 3;
    }
    return s["targets_met"].get<bool>() ? 0 : 1;
  }

  /// save: writes the current layout; restore: applies a saved layout if the current one differs.
  int layout(const std::string &command, const fs::path &file) {
    auto now = display::capture();
    if (!now) {
      std::fprintf(stderr, "ERROR: %s\n", now.error().to_string().c_str());
      return 1;
    }
    if (command == "save") {
      std::ofstream out(file, std::ios::binary | std::ios::trunc);
      out << display::to_json(now.value()).dump(2) << '\n';
      std::fputs(display::describe(now.value()).c_str(), stdout);
      return out ? 0 : 1;
    }
    std::ifstream in(file, std::ios::binary);
    const json j = json::parse(in, nullptr, false);
    auto saved = display::from_json(j);
    if (!saved) {
      std::fprintf(stderr, "ERROR: %s\n", saved.error().to_string().c_str());
      return 1;
    }
    if (display::compare(saved.value(), now.value()).empty()) {
      std::puts("layout already matches");
      return 0;
    }
    if (auto st = display::apply(saved.value()); !st) {
      std::fprintf(stderr, "ERROR: %s\n", st.error().to_string().c_str());
      return 1;
    }
    std::puts("layout restored");
    return 0;
  }

  int probe_only(const options_t &o) {
    const auto epoch = steady_t::now();
    probes_t probes(epoch, o.record_ms);
    display_watch_t watch;
    probes.start();
    std::printf("probing for %u s (Ctrl+C stops)\n", o.seconds);
    nap(o.seconds * 1000);
    probes.stop();
    std::error_code ec;
    fs::create_directories(o.out, ec);
    write_csv(o.out, {}, probes);
    json s = summarize(o, nullptr, {}, probes, {}, false);
    s["environment"] = watch.environment();
    std::ofstream(o.out / L"summary.json", std::ios::trunc) << s.dump(2) << '\n';
    std::printf("%s\n", s.dump(2).c_str());
    return 0;
  }

}  // namespace

int wmain(int argc, wchar_t **argv) {
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCtrlHandler(on_console_ctrl, TRUE);
  // Per-monitor DPI awareness, so CCD and window coordinates are physical pixels.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  if (const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); FAILED(hr)) {
    std::fprintf(stderr, "ERROR: %s\n", win32::hresult_error(hr, "CoInitializeEx").to_string().c_str());
    return 1;
  }
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    args.push_back(win32::to_utf8(argv[i]));
  }
  if (args.size() == 2 && (args[0] == "save" || args[0] == "restore")) {
    return layout(args[0], fs::path(argv[2]));
  }
  auto o = parse(args);
  if (!o) {
    std::fprintf(stderr, "ERROR: %s\n", o.error().to_string().c_str());
    usage();
    return 2;
  }
  // Measurements need the console display on and the session unlocked: no idle display-off or screen saver.
  if (o.value().command == "run" || o.value().command == "hold" || o.value().command == "probe") {
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
  }
  if (o.value().command == "run" || o.value().command == "hold") {
    return run(o.value());
  }
  if (o.value().command == "probe") {
    return probe_only(o.value());
  }
  usage();
  return 2;
}
