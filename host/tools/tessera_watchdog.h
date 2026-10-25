/**
 * @file tools/tessera_watchdog.h
 * @brief [fork] Tessera: service-side watchdog that undoes journaled host state changes after a host
 *        crash, a reboot, or when the service stops (brief §4.2). See host-helpers/common.
 */
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace tessera_watchdog {

  /**
   * @brief If the state journal has entries, re-runs this executable in the console session with
   *        `--tessera-restore` (display changes must be made from the interactive session) and waits for it.
   * @param console_token A primary token for the console session (as used to launch Sunshine.exe).
   * @param reason Logged, e.g. "service start".
   */
  void restore_if_needed(HANDLE console_token, const char *reason);

  /**
   * @brief Entry point of the `--tessera-restore` re-invocation inside the console session.
   * @return 0 if everything was restored, 1 otherwise.
   */
  int restore_main();

}  // namespace tessera_watchdog
