/**
 * @file tools/vdd-stress/src/probes.h
 * @brief Background probes that detect system-wide stalls while displays are added and removed.
 *
 * "No system-wide stall > 250 ms" (brief §4.5) is measured from three angles, because a display-driver freeze
 * shows up differently depending on what blocks:
 *   compose  a tiny topmost window is redrawn and DwmFlush() waits for the compositor to show it; a gap means
 *            DWM produced no frame (the screen froze).
 *   ui       WM_NULL round trips to Explorer's taskbar window; a gap means the shell's UI thread was blocked
 *            (typically on a display-configuration change).
 *   sched    a time-critical thread sleeping 1 ms; a gap means the CPU scheduler itself was starved (DPC/ISR
 *            storms, driver spin).
 * A CPU sampler records the load of dwm.exe, WUDFHost.exe (which hosts UMDF display drivers) and explorer.exe
 * once per second, to spot busy-waiting swap-chain threads.
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// local includes
#include "tessera/result.h"

namespace tessera::stress {

  using steady_t = std::chrono::steady_clock;

  /// Milliseconds between two time points.
  inline double ms_between(steady_t::time_point a, steady_t::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
  }

  struct gap_t {
    std::string probe;
    double t_ms = 0;  ///< Start of the gap, relative to the run start.
    double duration_ms = 0;
  };

  struct cpu_sample_t {
    double t_ms = 0;
    std::string process;  ///< Image name; all processes with that name are summed.
    double cpu_percent = 0;  ///< Of one logical CPU (can exceed 100).
    std::uint32_t instances = 0;
  };

  struct probe_stats_t {
    std::uint64_t samples = 0;  ///< Iterations completed.
    std::uint64_t errors = 0;  ///< Iterations whose measurement call failed.
    double max_ms = 0;  ///< Longest interval seen, recorded or not.
  };

  class probes_t {
  public:
    /// Gaps longer than `record_ms` are recorded.
    probes_t(steady_t::time_point epoch, double record_ms);
    ~probes_t();

    probes_t(const probes_t &) = delete;
    probes_t &operator=(const probes_t &) = delete;

    status_t start();
    void stop();

    /// Valid after stop().
    std::vector<gap_t> gaps() const;
    std::vector<cpu_sample_t> cpu() const;
    probe_stats_t stats(const std::string &probe) const;

  private:
    /// Counts one iteration of `probe` and records it as a gap if it took longer than the threshold.
    void sample(const char *probe, bool ok, steady_t::time_point start = {}, double duration_ms = 0);

    void run_sched();
    void run_compose();
    void run_ui();
    void run_cpu();

    steady_t::time_point epoch_;
    double record_ms_;
    std::atomic<bool> running_ {false};
    std::vector<std::thread> threads_;
    mutable std::mutex mutex_;
    std::vector<gap_t> gaps_;
    std::vector<cpu_sample_t> cpu_;
    std::vector<std::pair<std::string, probe_stats_t>> stats_;
    bool timer_period_set_ = false;
  };

}  // namespace tessera::stress
