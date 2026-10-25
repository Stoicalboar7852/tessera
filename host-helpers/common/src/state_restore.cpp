/**
 * @file host-helpers/common/src/state_restore.cpp
 * @brief Restore loop and the display topology handler.
 */
// local includes
#include "tessera/state_restore.h"

// standard includes
#include <algorithm>

namespace tessera::state {

  namespace {

    class display_topology_handler_t: public state_handler_t {
    public:
      std::string_view kind() const override {
        return "display_topology";
      }

      result_t<bool> is_restored(const journal_entry_t &entry) override {
        auto wanted = display::from_json(entry.before);
        if (!wanted) {
          return wanted.error();
        }
        auto now = display::capture();
        if (!now) {
          return now.error();
        }
        return display::compare(wanted.value(), now.value()).empty();
      }

      status_t restore(const journal_entry_t &entry, const restore_options_t &options) override {
        auto wanted = display::from_json(entry.before);
        if (!wanted) {
          return wanted.error();
        }
        auto st = display::apply(wanted.value());
        if (st || !options.display_fallback_to_database) {
          return st;
        }
        // Exact restore impossible: get the user a working desktop from Windows' own database instead.
        if (auto fb = display::apply_database_current(); !fb) {
          return make_error(tessera_code_e::invalid_data, "display_topology::restore", st.error().to_string() + "; fallback also failed: " + fb.error().to_string());
        }
        return make_error(tessera_code_e::not_found, "display_topology::restore", "exact restore failed, applied Windows' stored configuration instead: " + st.error().to_string());
      }
    };

  }  // namespace

  bool restore_report_t::all_ok() const {
    return journal_updated && std::none_of(results.begin(), results.end(), [](const restore_result_t &r) {
             return r.outcome == restore_outcome_e::failed;
           });
  }

  const char *outcome_name(restore_outcome_e o) {
    switch (o) {
      case restore_outcome_e::restored:
        return "restored";
      case restore_outcome_e::already_restored:
        return "already-restored";
      case restore_outcome_e::failed:
        return "FAILED";
      case restore_outcome_e::skipped:
        return "skipped";
    }
    return "?";
  }

  std::vector<std::unique_ptr<state_handler_t>> default_handlers() {
    std::vector<std::unique_ptr<state_handler_t>> h;
    h.push_back(std::make_unique<display_topology_handler_t>());
    return h;
  }

  result_t<restore_report_t> restore_all(state_journal_t &journal, std::vector<std::unique_ptr<state_handler_t>> &handlers, const restore_options_t &options) {
    auto snap = journal.read();
    if (!snap) {
      return snap.error();
    }
    restore_report_t report;
    report.warnings = snap->warnings;
    auto entries = snap->doc.entries;
    // Newest first: later changes were made on top of earlier ones.
    std::sort(entries.begin(), entries.end(), [](const journal_entry_t &a, const journal_entry_t &b) {
      return a.created_unix_ms > b.created_unix_ms;
    });
    for (const auto &e : entries) {
      restore_result_t r {e.id, e.kind, e.owner, restore_outcome_e::skipped, {}};
      if (options.only_kind && *options.only_kind != e.kind) {
        r.message = "filtered out";
        report.results.push_back(std::move(r));
        continue;
      }
      const auto it = std::find_if(handlers.begin(), handlers.end(), [&](const auto &h) {
        return h->kind() == e.kind;
      });
      if (it == handlers.end()) {
        r.message = "no handler for this kind in this build; entry kept";
        report.results.push_back(std::move(r));
        continue;
      }
      auto done = (*it)->is_restored(e);
      if (!done) {
        r.outcome = restore_outcome_e::failed;
        r.message = done.error().to_string();
        report.results.push_back(std::move(r));
        continue;
      }
      if (options.dry_run) {
        r.message = done.value() ? "already matches the saved state" : "would restore";
        report.results.push_back(std::move(r));
        continue;
      }
      if (done.value()) {
        r.outcome = restore_outcome_e::already_restored;
      } else if (auto st = (*it)->restore(e, options); st) {
        r.outcome = restore_outcome_e::restored;
      } else {
        r.outcome = restore_outcome_e::failed;
        r.message = st.error().to_string();
        report.results.push_back(std::move(r));
        continue;
      }
      if (auto rm = journal.remove(e.id); !rm) {
        report.journal_updated = false;
        r.message = "restored, but the entry could not be removed: " + rm.error().to_string();
      }
      report.results.push_back(std::move(r));
    }
    return report;
  }

  result_t<std::string> journaled_display_change(state_journal_t &journal, std::string_view owner, const display::topology_t &wanted) {
    auto before = display::capture();
    if (!before) {
      return before.error();
    }
    if (before->paths.empty()) {
      return make_error(tessera_code_e::unsafe, "journaled_display_change", "no active display to start from");
    }
    auto id = journal.begin("display_topology", owner, display::to_json(before.value()));
    if (!id) {
      return id.error();  // nothing changed yet
    }
    auto applied = display::apply(wanted);
    auto count = display::active_path_count();
    if (!applied || !count || count.value() == 0) {
      // Undo right away; the journal entry stays until the undo is confirmed.
      std::string why = !applied ? applied.error().to_string() : (!count ? count.error().to_string() : "no active display after the change");
      if (auto undo = display::apply(before.value()); !undo) {
        if (auto fb = display::apply_database_current(); !fb) {
          return make_error(tessera_code_e::unsafe, "journaled_display_change", why + "; undo failed: " + undo.error().to_string() + "; fallback failed: " + fb.error().to_string());
        }
      }
      // If removal fails the entry stays; restoring it later is a no-op because the system already matches.
      const auto removed = journal.remove(id.value());
      return make_error(tessera_code_e::unsafe, "journaled_display_change", "change reverted: " + why + (removed ? "" : "; journal entry kept: " + removed.error().to_string()));
    }
    if (auto st = journal.mark_applied(id.value(), display::to_json(wanted)); !st) {
      return st.error();  // entry stays pending, which restore handles the same way
    }
    return id;
  }

}  // namespace tessera::state
