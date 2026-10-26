/**
 * @file tools/vdd-stress/src/main.cpp
 * @brief vdd-stress.exe: adds and removes virtual displays in cycles and measures how long each change takes
 *        and whether the system stalls meanwhile (brief §4.5, Phase 1).
 *
 *   vdd-stress run   --backend B [--counts 1,2,4,8] [--cycles 10] [--mode 1920x1080@60] [--hold-ms 2000]
 *                    [--settle-ms 500] [--timeout-ms 10000] [--extend-after-ms 4000] [--record-ms 50]
 *                    [--out DIR] [--mtt-config PATH]
 *   vdd-stress hold  --backend B --count N --seconds S [--mode ...] [--out DIR]   (steady state with N displays)
 *   vdd-stress probe --seconds S [--out DIR]                                     (baseline, no driver)
 *   vdd-stress save FILE | restore FILE                                          (display layout as JSON)
 *
 * Each cycle adds `count` displays one by one (or restarts a fixed-count driver with `count` displays), waits
 * until each is active in the CCD topology, holds, then removes them again. The display layout from before the
 * run is restored at the end. Output: events.csv, gaps.csv, cpu.csv and summary.json in --out.
 * Exit codes: 0 targets met, 1 targets missed or an operation failed, 2 usage, 3 aborted.
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// platform includes
#include <windows.h>
#include <objbase.h>

// local includes
#include "backends.h"
#include "probes.h"
#include "tessera/display_topology.h"
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
  };

  struct event_t {
    double t_ms = 0;
    std::string op;  ///< add, remove, set_count
    std::uint32_t count = 0;  ///< Displays in this cycle.
    std::uint32_t cycle = 0;
    std::uint32_t index = 0;  ///< Display number within the cycle (add/remove), or the new count (set_count).
    double call_ms = 0;  ///< Time inside the driver call.
    double visible_ms = -1;  ///< From the call until the CCD topology showed the change; -1 = never.
    double end_ms = 0;
    bool ok = false;
    std::string detail;
  };

  void usage() {
    std::puts(
      "Usage:\n"
      "  vdd-stress run   --backend sudovda|parsec|mtt [--counts 1,2,4,8] [--cycles 10] [--mode 1920x1080@60]\n"
      "                   [--hold-ms 2000] [--settle-ms 500] [--timeout-ms 10000] [--extend-after-ms 4000]\n"
      "                   [--record-ms 50] [--out DIR] [--mtt-config PATH]\n"
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
  double wait_active(std::size_t wanted, steady_t::time_point since, const options_t &o, std::string &detail) {
    bool extended = false;
    while (!g_abort) {
      auto n = display::active_path_count();
      const double elapsed = ms_between(since, steady_t::now());
      if (n && n.value() == wanted) {
        return elapsed;
      }
      if (elapsed > o.timeout_ms) {
        detail += (detail.empty() ? "" : "; ") + std::string("timeout with ") + (n ? std::to_string(n.value()) : "?") + " active paths, wanted " + std::to_string(wanted);
        return -1;
      }
      if (!extended && n && n.value() < wanted && elapsed > o.extend_after_ms) {
        extended = true;
        const LONG rc = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_EXTEND);
        detail += (detail.empty() ? "" : "; ") + std::string("asked Windows to extend (rc ") + std::to_string(rc) + ")";
      }
      Sleep(25);
    }
    detail += "aborted";
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
    ev << "t_ms,op,count,cycle,index,call_ms,visible_ms,ok,detail\n";
    for (const auto &e : events) {
      std::string detail = e.detail;
      std::replace(detail.begin(), detail.end(), ',', ';');
      ev << e.t_ms << ',' << e.op << ',' << e.count << ',' << e.cycle << ',' << e.index << ',' << e.call_ms << ',' << e.visible_ms << ',' << (e.ok ? 1 : 0) << ',' << detail << '\n';
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
    const std::size_t base = before.value().paths.size();
    if (base == 0) {
      std::fputs("ERROR: no active display; refusing to start\n", stderr);
      return 1;
    }
    std::printf("%s: %zu active display(s) before the run; mode %s\n", backend.name(), base, o.mode.to_string().c_str());
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
    {
      keepalive_t keepalive(backend, keepalive_errors);
      auto timed = [&](const std::string &op, std::uint32_t n, std::uint32_t cycle, std::uint32_t index, std::size_t wanted, auto &&call) {
        event_t e;
        e.t_ms = ms_between(epoch, steady_t::now());
        e.op = op;
        e.count = n;
        e.cycle = cycle;
        e.index = index;
        const auto t0 = steady_t::now();
        const status_t st = call();
        e.call_ms = ms_between(t0, steady_t::now());
        if (st) {
          e.visible_ms = wait_active(wanted, t0, o, e.detail);
          e.ok = e.visible_ms >= 0;
        } else {
          e.detail = st.error().to_string();
        }
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
          if (backend.dynamic()) {
            for (std::uint32_t i = 1; i <= n && ok && !g_abort; ++i) {
              ok = timed("add", n, c, i, base + i, [&]() -> status_t {
                auto h = backend.add(o.mode);
                if (!h) {
                  return h.error();
                }
                handles.push_back(h.value());
                return status_t::ok();
              });
            }
            if (ok) {
              nap(o.hold_ms);
            }
            for (std::uint32_t i = static_cast<std::uint32_t>(handles.size()); i >= 1 && !g_abort; --i) {
              const std::uint32_t h = handles.back();
              handles.pop_back();
              ok = timed("remove", n, c, i, base + i - 1, [&] {
                return backend.remove(h);
              }) && ok;
            }
          } else {
            count_active = true;
            ok = timed("set_count", n, c, n, base + n, [&] {
              return backend.set_count(n, o.mode);
            });
            nap(o.hold_ms);
            ok = timed("set_count", n, c, 0, base, [&] {
              return backend.set_count(0, o.mode);
            }) && ok;
            count_active = !ok;
          }
          std::printf("count %u cycle %u/%u %s\n", n, c, o.cycles, ok ? "ok" : "FAILED");
          std::fflush(stdout);
          nap(o.settle_ms);
        }
      }
      cleanup(backend, handles, count_active, before.value(), base, o);
    }
    probes.stop();

    std::error_code ec;
    fs::create_directories(o.out, ec);
    write_csv(o.out, events, probes);
    const json s = summarize(o, &backend, events, probes, keepalive_errors, g_abort);
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
    probes.start();
    std::printf("probing for %u s (Ctrl+C stops)\n", o.seconds);
    nap(o.seconds * 1000);
    probes.stop();
    std::error_code ec;
    fs::create_directories(o.out, ec);
    write_csv(o.out, {}, probes);
    const json s = summarize(o, nullptr, {}, probes, {}, false);
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
  if (o.value().command == "run" || o.value().command == "hold") {
    return run(o.value());
  }
  if (o.value().command == "probe") {
    return probe_only(o.value());
  }
  usage();
  return 2;
}
