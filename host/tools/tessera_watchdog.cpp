/**
 * @file tools/tessera_watchdog.cpp
 * @brief [fork] Tessera: implementation of the service-side restore watchdog.
 */
#include "tessera_watchdog.h"

// standard includes
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

// local includes
#include "tessera/state_journal.h"
#include "tessera/state_restore.h"
#include "tessera/win32.h"

namespace fs = std::filesystem;

namespace tessera_watchdog {

  namespace {

    constexpr DWORD restore_timeout_ms = 60'000;

    /// Appends one line to %ProgramData%\Tessera\logs\watchdog.log (best effort; never throws).
    void log_line(const std::string &text) {
      try {
        auto base = tessera::win32::program_data_dir();
        if (!base) {
          return;
        }
        const fs::path dir = base.value() / L"Tessera" / L"logs";
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::ofstream out(dir / L"watchdog.log", std::ios::app | std::ios::binary);
        SYSTEMTIME st;
        GetLocalTime(&st);
        char ts[32];
        std::snprintf(ts, sizeof(ts), "%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        out << ts << " [pid " << GetCurrentProcessId() << "] " << text << "\n";
      } catch (...) {
        // logging must never take the service down
      }
    }

    bool journal_has_entries() {
      auto path = tessera::state::state_journal_t::default_path();
      if (!path) {
        log_line("cannot locate journal: " + path.error().to_string());
        return false;
      }
      tessera::state::state_journal_t journal({path.value()});
      auto snap = journal.read();
      if (!snap) {
        // Unreadable journal: let the restore process report it in detail.
        log_line("journal unreadable: " + snap.error().to_string());
        return true;
      }
      return !snap->doc.entries.empty();
    }

  }  // namespace

  void restore_if_needed(HANDLE console_token, const char *reason) {
    if (!journal_has_entries()) {
      return;
    }
    log_line(std::string("journal has entries; restoring (") + reason + ")");

    std::array<wchar_t, MAX_PATH> module_path {};
    if (GetModuleFileNameW(nullptr, module_path.data(), static_cast<DWORD>(module_path.size())) == 0) {
      log_line("GetModuleFileNameW failed: " + tessera::win32::last_error("GetModuleFileNameW").to_string());
      return;
    }
    std::wstring command = L"\"" + std::wstring(module_path.data()) + L"\" --tessera-restore";

    STARTUPINFOW startup_info {};
    startup_info.cb = sizeof(startup_info);
    startup_info.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
    PROCESS_INFORMATION process_info {};
    if (!CreateProcessAsUserW(console_token, module_path.data(), command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, nullptr, nullptr, &startup_info, &process_info)) {
      log_line("CreateProcessAsUserW(--tessera-restore) failed: " + tessera::win32::last_error("CreateProcessAsUserW").to_string());
      return;
    }
    tessera::win32::unique_handle_t process(process_info.hProcess);
    tessera::win32::unique_handle_t thread(process_info.hThread);

    const DWORD wait = WaitForSingleObject(process.get(), restore_timeout_ms);
    if (wait != WAIT_OBJECT_0) {
      log_line("restore process did not finish within 60 s; terminating it");
      TerminateProcess(process.get(), ERROR_TIMEOUT);
      return;
    }
    DWORD exit_code = 0;
    GetExitCodeProcess(process.get(), &exit_code);
    log_line("restore process exited with " + std::to_string(exit_code));
  }

  int restore_main() {
    // Act on the input desktop (it is the Winlogon desktop at the lock or logon screen); SYSTEM may open it.
    if (HDESK desk = OpenInputDesktop(0, FALSE, GENERIC_ALL); desk != nullptr) {
      if (!SetThreadDesktop(desk)) {
        log_line("SetThreadDesktop failed: " + tessera::win32::last_error("SetThreadDesktop").to_string());
      }
    }

    auto path = tessera::state::state_journal_t::default_path();
    if (!path) {
      log_line("cannot locate journal: " + path.error().to_string());
      return 1;
    }
    tessera::state::state_journal_t journal({path.value()});
    auto handlers = tessera::state::default_handlers();
    tessera::state::restore_options_t options;
    options.display_fallback_to_database = true;  // a usable desktop beats an exact but impossible layout
    auto report = tessera::state::restore_all(journal, handlers, options);
    if (!report) {
      log_line("restore failed: " + report.error().to_string());
      return 1;
    }
    for (const auto &w : report->warnings) {
      log_line("warning: " + w);
    }
    for (const auto &r : report->results) {
      log_line(std::string(tessera::state::outcome_name(r.outcome)) + " " + r.kind + " " + r.id + " owner=" + r.owner + (r.message.empty() ? "" : " " + r.message));
    }
    return report->all_ok() ? 0 : 1;
  }

}  // namespace tessera_watchdog
