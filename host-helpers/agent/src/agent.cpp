/**
 * @file host-helpers/agent/src/agent.cpp
 * @brief tessera-agent.exe: runs in the interactive user session. Phase 0 scope: the emergency hotkey
 *        (default Ctrl+Alt+Shift+F12) that restores every journaled change (brief §4.2).
 *
 * The restore runs in-process: display changes need the interactive session, which this process is in.
 * If the journal directory has been secured by the service, entries cannot be removed from here; the
 * service's next pass sees them as already restored and removes them.
 * The hotkey cannot fire while the secure desktop (lock screen, UAC prompt) is shown; recover.exe and the
 * service watchdog cover that case.
 */
// standard includes
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

// local includes
#include "tessera/state_journal.h"
#include "tessera/state_restore.h"
#include "tessera/win32.h"

namespace fs = std::filesystem;
using namespace tessera;

namespace {

  constexpr int hotkey_id = 0x7E55;  // arbitrary, unique within this process
  constexpr UINT msg_restore_done = WM_APP + 1;

  std::atomic<bool> restoring {false};

  fs::path log_path() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    fs::path dir = (n > 0 && n < MAX_PATH) ? fs::path(buf) : fs::temp_directory_path();
    dir /= L"Tessera";
    dir /= L"logs";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir / L"agent.log";
  }

  void log_line(const std::string &text) {
    std::ofstream out(log_path(), std::ios::app | std::ios::binary);
    SYSTEMTIME st;
    GetLocalTime(&st);
    char ts[32];
    std::snprintf(ts, sizeof(ts), "%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    out << ts << " " << text << "\n";
  }

  /// Parses "ctrl+alt+shift+f12" style hotkeys. Returns false on unknown tokens.
  bool parse_hotkey(const std::string &spec, UINT &mods, UINT &vk) {
    mods = MOD_NOREPEAT;
    vk = 0;
    std::string s;
    for (char c : spec) {
      s.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
    }
    std::size_t start = 0;
    while (start <= s.size()) {
      const auto end = s.find('+', start);
      const std::string tok = s.substr(start, end == std::string::npos ? std::string::npos : end - start);
      if (tok == "ctrl" || tok == "control") {
        mods |= MOD_CONTROL;
      } else if (tok == "alt") {
        mods |= MOD_ALT;
      } else if (tok == "shift") {
        mods |= MOD_SHIFT;
      } else if (tok == "win") {
        mods |= MOD_WIN;
      } else if (tok.size() >= 2 && tok[0] == 'f' && std::isdigit(static_cast<unsigned char>(tok[1]))) {
        const int n = std::atoi(tok.c_str() + 1);
        if (n < 1 || n > 24) {
          return false;
        }
        vk = VK_F1 + static_cast<UINT>(n - 1);
      } else if (tok.size() == 1 && ((tok[0] >= 'a' && tok[0] <= 'z') || (tok[0] >= '0' && tok[0] <= '9'))) {
        vk = static_cast<UINT>(tok[0] >= 'a' ? tok[0] - 'a' + 'A' : tok[0]);
      } else {
        return false;
      }
      if (end == std::string::npos) {
        break;
      }
      start = end + 1;
    }
    return vk != 0 && (mods & ~MOD_NOREPEAT) != 0;
  }

  void restore_async(HWND hwnd) {
    if (restoring.exchange(true)) {
      log_line("hotkey pressed while a restore is running; ignored");
      return;
    }
    std::thread([hwnd] {
      bool ok = false;
      auto path = state::state_journal_t::default_path();
      if (!path) {
        log_line("cannot locate journal: " + path.error().to_string());
      } else {
        state::state_journal_t journal({path.value()});
        auto handlers = state::default_handlers();
        state::restore_options_t opts;
        opts.display_fallback_to_database = true;
        auto report = state::restore_all(journal, handlers, opts);
        if (!report) {
          log_line("restore failed: " + report.error().to_string());
        } else {
          for (const auto &w : report->warnings) {
            log_line("warning: " + w);
          }
          for (const auto &r : report->results) {
            log_line(std::string(state::outcome_name(r.outcome)) + " " + r.kind + " " + r.id + (r.message.empty() ? "" : " " + r.message));
          }
          if (report->results.empty()) {
            log_line("nothing to restore");
          }
          ok = report->all_ok();
        }
      }
      PostMessageW(hwnd, msg_restore_done, ok ? 1 : 0, 0);
    }).detach();
  }

  LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
      case WM_HOTKEY:
        if (wp == hotkey_id) {
          log_line("emergency hotkey pressed: restoring journaled state");
          restore_async(hwnd);
        }
        return 0;
      case msg_restore_done:
        restoring = false;
        MessageBeep(wp ? MB_OK : MB_ICONHAND);
        return 0;
      case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
      default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
  }

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdline, int) {
  // Single instance per session.
  win32::unique_handle_t mutex(CreateMutexW(nullptr, TRUE, L"Local\\Tessera.Agent"));
  if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
    return 0;
  }

  std::string hotkey = "ctrl+alt+shift+f12";
  const std::string args = win32::to_utf8(cmdline != nullptr ? cmdline : L"");
  if (const auto pos = args.find("--hotkey "); pos != std::string::npos) {
    hotkey = args.substr(pos + 9);
    if (const auto sp = hotkey.find(' '); sp != std::string::npos) {
      hotkey.resize(sp);
    }
  }
  UINT mods = 0;
  UINT vk = 0;
  if (!parse_hotkey(hotkey, mods, vk)) {
    log_line("invalid hotkey '" + hotkey + "'; using ctrl+alt+shift+f12");
    parse_hotkey("ctrl+alt+shift+f12", mods, vk);
    hotkey = "ctrl+alt+shift+f12";
  }

  WNDCLASSW wc {};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = instance;
  wc.lpszClassName = L"TesseraAgent";
  if (!RegisterClassW(&wc)) {
    log_line("RegisterClassW failed: " + win32::last_error("RegisterClassW").to_string());
    return 1;
  }
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Tessera agent", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
  if (hwnd == nullptr) {
    log_line("CreateWindowExW failed: " + win32::last_error("CreateWindowExW").to_string());
    return 1;
  }
  if (!RegisterHotKey(hwnd, hotkey_id, mods, vk)) {
    log_line("RegisterHotKey(" + hotkey + ") failed (already used by another program?): " + win32::last_error("RegisterHotKey").to_string());
    return 1;
  }
  log_line("agent started; emergency restore hotkey " + hotkey);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  UnregisterHotKey(hwnd, hotkey_id);
  return 0;
}
