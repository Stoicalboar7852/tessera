/**
 * @file tools/recover/src/topoctl.cpp
 * @brief topoctl.exe: developer/test tool to inspect the display topology and to make deliberate,
 *        journaled topology changes for the recovery integration test.
 */
// standard includes
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// local includes
#include "tessera/display_topology.h"
#include "tessera/state_journal.h"
#include "tessera/state_restore.h"
#include "tessera/win32.h"

namespace fs = std::filesystem;
using namespace tessera;
using nlohmann::json;

namespace {

  void usage() {
    std::puts(
      "Usage:\n"
      "  topoctl show\n"
      "  topoctl save FILE\n"
      "  topoctl compare FILE                      exit 0 if the current layout equals FILE\n"
      "  topoctl change SCENARIO [--journal PATH] [--owner NAME]\n"
      "\n"
      "Scenarios (each is journaled, so recover.exe can undo it):\n"
      "  move-secondary     move a non-primary display to the far left of the layout\n"
      "  swap-primary       make a non-primary display the primary\n"
      "  refresh            change the primary display's refresh rate\n"
      "  disable-secondary  turn off a non-primary display (like monitor blanking)"
    );
  }

  void print_topology(const display::topology_t &t) {
    std::fputs(display::describe(t).c_str(), stdout);
  }

  const display::path_t *first_secondary(const display::topology_t &t) {
    for (const auto &p : t.paths) {
      if (!p.is_primary()) {
        return &p;
      }
    }
    return nullptr;
  }

  result_t<display::topology_t> scenario(const std::string &name, display::topology_t t) {
    auto need_two = [&]() -> status_t {
      if (t.paths.size() < 2 || first_secondary(t) == nullptr) {
        return make_error(tessera_code_e::unsupported, "topoctl", "scenario '" + name + "' needs at least two active displays");
      }
      return status_t::ok();
    };
    if (name == "move-secondary") {
      if (auto st = need_two(); !st) {
        return st.error();
      }
      auto &p = *std::find_if(t.paths.begin(), t.paths.end(), [](const auto &x) {
        return !x.is_primary();
      });
      // Put it left of everything else, top-aligned with the leftmost other display.
      const display::path_t *leftmost = nullptr;
      for (const auto &o : t.paths) {
        if (&o != &p && (leftmost == nullptr || o.position_x < leftmost->position_x)) {
          leftmost = &o;
        }
      }
      p.position_x = leftmost->position_x - static_cast<std::int32_t>(p.width);
      p.position_y = leftmost->position_y;
      return t;
    }
    if (name == "swap-primary") {
      if (auto st = need_two(); !st) {
        return st.error();
      }
      const auto *q = first_secondary(t);
      const std::int32_t dx = q->position_x;
      const std::int32_t dy = q->position_y;
      for (auto &p : t.paths) {
        p.position_x -= dx;
        p.position_y -= dy;
      }
      return t;
    }
    if (name == "disable-secondary") {
      if (auto st = need_two(); !st) {
        return st.error();
      }
      t.paths.erase(std::find_if(t.paths.begin(), t.paths.end(), [](const auto &x) {
        return !x.is_primary();
      }));
      return t;
    }
    if (name == "refresh") {
      auto it = std::find_if(t.paths.begin(), t.paths.end(), [](const auto &x) {
        return x.is_primary();
      });
      if (it == t.paths.end()) {
        return make_error(tessera_code_e::not_found, "topoctl", "no primary display");
      }
      auto &p = *it;
      const double current = p.refresh_rate.denominator ? static_cast<double>(p.refresh_rate.numerator) / p.refresh_rate.denominator : 0.0;
      const std::wstring gdi = win32::to_wide(p.gdi_name);
      DEVMODEW dm {};
      dm.dmSize = sizeof(dm);
      std::uint32_t best = 0;
      for (DWORD i = 0; EnumDisplaySettingsW(gdi.c_str(), i, &dm); ++i) {
        if (dm.dmPelsWidth == p.width && dm.dmPelsHeight == p.height && dm.dmDisplayFrequency > 1 &&
            std::abs(static_cast<double>(dm.dmDisplayFrequency) - current) > 1.5 && dm.dmDisplayFrequency > best) {
          best = dm.dmDisplayFrequency;
        }
      }
      if (best == 0) {
        return make_error(tessera_code_e::unsupported, "topoctl", "the primary display offers no other refresh rate at " + std::to_string(p.width) + "x" + std::to_string(p.height));
      }
      p.refresh_rate = {best, 1};
      p.timing.reset();  // let Windows pick the timing for the new rate
      return t;
    }
    return make_error(tessera_code_e::invalid_data, "topoctl", "unknown scenario '" + name + "'");
  }

  int fail(const error_t &e) {
    std::fprintf(stderr, "ERROR: %s\n", e.to_string().c_str());
    return 1;
  }

}  // namespace

int wmain(int argc, wchar_t **argv) {
  SetConsoleOutputCP(CP_UTF8);
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    args.push_back(win32::to_utf8(argv[i]));
  }
  if (args.empty()) {
    usage();
    return 2;
  }
  const std::string cmd = args[0];

  auto now = display::capture();
  if (!now) {
    return fail(now.error());
  }

  if (cmd == "show") {
    print_topology(now.value());
    return 0;
  }
  if ((cmd == "save" || cmd == "compare") && args.size() == 2) {
    const fs::path file(win32::to_wide(args[1]));
    if (cmd == "save") {
      std::ofstream out(file, std::ios::binary | std::ios::trunc);
      out << display::to_json(now.value()).dump(2) << "\n";
      if (!out) {
        std::fprintf(stderr, "ERROR: cannot write %s\n", args[1].c_str());
        return 1;
      }
      print_topology(now.value());
      return 0;
    }
    std::ifstream in(file, std::ios::binary);
    const std::string text {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const json j = json::parse(text, nullptr, false);
    auto saved = display::from_json(j);
    if (!saved) {
      return fail(saved.error());
    }
    const auto diffs = display::compare(saved.value(), now.value());
    if (diffs.empty()) {
      std::puts("MATCH: current layout equals the saved layout");
      return 0;
    }
    std::puts("DIFFERENT:");
    for (const auto &d : diffs) {
      std::printf("  %s\n", d.c_str());
    }
    return 1;
  }
  if (cmd == "change" && args.size() >= 2) {
    std::string owner = "topoctl";
    std::optional<fs::path> journal_path;
    for (std::size_t i = 2; i < args.size(); ++i) {
      if (args[i] == "--journal" && i + 1 < args.size()) {
        journal_path = fs::path(win32::to_wide(args[++i]));
      } else if (args[i] == "--owner" && i + 1 < args.size()) {
        owner = args[++i];
      } else {
        usage();
        return 2;
      }
    }
    if (!journal_path) {
      auto def = state::state_journal_t::default_path();
      if (!def) {
        return fail(def.error());
      }
      journal_path = def.value();
    }
    auto wanted = scenario(args[1], now.value());
    if (!wanted) {
      return fail(wanted.error());
    }
    state::state_journal_t journal({*journal_path});
    auto id = state::journaled_display_change(journal, owner, wanted.value());
    if (!id) {
      return fail(id.error());
    }
    std::printf("changed (journal entry %s). New layout:\n", id.value().c_str());
    auto after = display::capture();
    if (after) {
      print_topology(after.value());
    }
    return 0;
  }
  usage();
  return 2;
}
