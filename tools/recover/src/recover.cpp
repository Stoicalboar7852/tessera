/**
 * @file tools/recover/src/recover.cpp
 * @brief recover.exe: lists and restores journaled host state changes (brief §4.2).
 *
 * Works without the Tessera service. Restoring the display topology needs no elevation when run in the
 * interactive session; later handlers (shell, registry, services) will need it and say so.
 */
// standard includes
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// local includes
#include "tessera/display_topology.h"
#include "tessera/state_journal.h"
#include "tessera/state_restore.h"
#include "tessera/win32.h"

namespace fs = std::filesystem;
using namespace tessera;

namespace {

  enum exit_code_e : int {
    exit_ok = 0,
    exit_restore_failed = 1,
    exit_usage = 2,
    exit_journal_unreadable = 3,
  };

  /// Writes to stdout and, best effort, to a log file.
  class log_t {
  public:
    log_t() {
      for (auto base : {win32::program_data_dir(), local_app_data()}) {
        if (!base) {
          continue;
        }
        const fs::path dir = base.value() / L"Tessera" / L"logs";
        std::error_code ec;
        fs::create_directories(dir, ec);
        file_.open(dir / L"recover.log", std::ios::app | std::ios::binary);
        if (file_) {
          path_ = dir / L"recover.log";
          break;
        }
      }
    }

    void line(const std::string &text) {
      std::fwrite(text.data(), 1, text.size(), stdout);
      std::fputc('\n', stdout);
      if (file_) {
        file_ << timestamp() << " " << text << "\n";
        file_.flush();
      }
    }

    const fs::path &path() const {
      return path_;
    }

  private:
    static result_t<fs::path> local_app_data() {
      wchar_t buf[MAX_PATH];
      const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
      if (n == 0 || n >= MAX_PATH) {
        return win32::last_error("GetEnvironmentVariableW(LOCALAPPDATA)");
      }
      return fs::path(buf);
    }

    static std::string timestamp() {
      SYSTEMTIME st;
      GetLocalTime(&st);
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
      return buf;
    }

    std::ofstream file_;
    fs::path path_;
  };

  void usage() {
    std::puts(
      "Usage:\n"
      "  recover [--journal PATH] --list\n"
      "  recover [--journal PATH] --restore [--kind KIND] [--dry-run] [--fallback]\n"
      "\n"
      "  --list       Show journaled changes that have not been undone.\n"
      "  --restore    Undo them, newest first, and remove each entry once undone.\n"
      "  --kind KIND  Only handle entries of this kind (e.g. display_topology).\n"
      "  --dry-run    Show what would be restored without changing anything.\n"
      "  --fallback   If a display layout cannot be restored exactly (monitor unplugged), apply\n"
      "               Windows' stored layout for the connected monitors instead.\n"
      "  --journal    Journal file (default %ProgramData%\\Tessera\\state-journal.json).\n"
      "\n"
      "Exit codes: 0 ok, 1 some restores failed, 2 usage error, 3 journal unreadable."
    );
  }

  std::string format_time(std::int64_t unix_ms) {
    const std::time_t t = static_cast<std::time_t>(unix_ms / 1000);
    std::tm tm {};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
  }

  int list(state::state_journal_t &journal, log_t &log) {
    auto snap = journal.read();
    if (!snap) {
      log.line("ERROR: " + snap.error().to_string());
      return exit_journal_unreadable;
    }
    for (const auto &w : snap->warnings) {
      log.line("warning: " + w);
    }
    log.line("journal " + win32::to_utf8(journal.path().native()) + " generation " + std::to_string(snap->doc.generation) + ", " + std::to_string(snap->doc.entries.size()) + " entr" + (snap->doc.entries.size() == 1 ? "y" : "ies"));
    for (const auto &e : snap->doc.entries) {
      log.line("- " + e.id + "  " + e.kind + "  " + (e.state == state::entry_state_e::applied ? "applied" : "pending") + "  owner=" + e.owner + "  " + format_time(e.created_unix_ms));
      if (e.kind == "display_topology") {
        if (auto t = display::from_json(e.before)) {
          std::string d = display::describe(t.value());
          log.line("  saved layout:");
          std::size_t start = 0;
          while (start < d.size()) {
            const auto end = d.find('\n', start);
            log.line("    " + d.substr(start, end - start));
            start = end == std::string::npos ? d.size() : end + 1;
          }
        } else {
          log.line("  saved layout unreadable: " + t.error().to_string());
        }
      }
    }
    return exit_ok;
  }

  int restore(state::state_journal_t &journal, const state::restore_options_t &opts, log_t &log) {
    auto handlers = state::default_handlers();
    auto report = state::restore_all(journal, handlers, opts);
    if (!report) {
      log.line("ERROR: " + report.error().to_string());
      return exit_journal_unreadable;
    }
    for (const auto &w : report->warnings) {
      log.line("warning: " + w);
    }
    if (report->results.empty()) {
      log.line("nothing to restore");
      return exit_ok;
    }
    for (const auto &r : report->results) {
      log.line(std::string(state::outcome_name(r.outcome)) + "  " + r.kind + "  " + r.id + "  owner=" + r.owner + (r.message.empty() ? "" : "  " + r.message));
    }
    return report->all_ok() ? exit_ok : exit_restore_failed;
  }

}  // namespace

int wmain(int argc, wchar_t **argv) {
  SetConsoleOutputCP(CP_UTF8);
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    args.push_back(win32::to_utf8(argv[i]));
  }

  bool do_list = false;
  bool do_restore = false;
  state::restore_options_t opts;
  std::optional<fs::path> journal_path;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const auto &a = args[i];
    if (a == "--list") {
      do_list = true;
    } else if (a == "--restore") {
      do_restore = true;
    } else if (a == "--dry-run") {
      opts.dry_run = true;
    } else if (a == "--fallback") {
      opts.display_fallback_to_database = true;
    } else if (a == "--kind" && i + 1 < args.size()) {
      opts.only_kind = args[++i];
    } else if (a == "--journal" && i + 1 < args.size()) {
      journal_path = fs::path(win32::to_wide(args[++i]));
    } else if (a == "--help" || a == "-h" || a == "/?") {
      usage();
      return exit_ok;
    } else {
      std::fprintf(stderr, "unknown or incomplete argument: %s\n\n", a.c_str());
      usage();
      return exit_usage;
    }
  }
  if (do_list == do_restore) {
    usage();
    return exit_usage;
  }

  log_t log;
  if (!journal_path) {
    auto def = state::state_journal_t::default_path();
    if (!def) {
      log.line("ERROR: " + def.error().to_string());
      return exit_journal_unreadable;
    }
    journal_path = def.value();
  }
  state::state_journal_t journal({*journal_path});
  log.line(std::string("recover ") + (do_list ? "--list" : (opts.dry_run ? "--restore --dry-run" : "--restore")) + " (log: " + win32::to_utf8(log.path().native()) + ")");
  return do_list ? list(journal, log) : restore(journal, opts, log);
}
