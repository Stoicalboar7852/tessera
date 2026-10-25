/**
 * @file host-helpers/common/include/tessera/state_restore.h
 * @brief Handlers that undo journaled changes, and the restore loop used by the service watchdog,
 *        the emergency hotkey and recover.exe.
 */
#pragma once

// standard includes
#include <memory>
#include <optional>
#include <string>
#include <vector>

// local includes
#include "tessera/display_topology.h"
#include "tessera/state_journal.h"

namespace tessera::state {

  struct restore_options_t {
    bool dry_run = false;
    std::optional<std::string> only_kind;
    /// When an exact display restore is impossible (e.g. a monitor was unplugged), ask Windows to apply
    /// its stored configuration for the connected monitors, so the user still gets a working desktop.
    bool display_fallback_to_database = false;
  };

  /**
   * @brief Undoes one kind of journaled change. Restores must be idempotent: an entry may be pending
   *        (change possibly not applied) or may already have been restored by an earlier, interrupted run.
   */
  class state_handler_t {
  public:
    virtual ~state_handler_t() = default;

    virtual std::string_view kind() const = 0;

    /// True if the system already matches `entry.before`.
    virtual result_t<bool> is_restored(const journal_entry_t &entry) = 0;

    /// Puts `entry.before` back.
    virtual status_t restore(const journal_entry_t &entry, const restore_options_t &options) = 0;
  };

  enum class restore_outcome_e {
    restored,
    already_restored,
    failed,
    skipped,  ///< Filtered out, dry run, or no handler for this kind.
  };

  struct restore_result_t {
    std::string id;
    std::string kind;
    std::string owner;
    restore_outcome_e outcome = restore_outcome_e::skipped;
    std::string message;
  };

  struct restore_report_t {
    std::vector<restore_result_t> results;
    std::vector<std::string> warnings;  ///< From reading the journal (e.g. recovered from backup).
    bool journal_updated = true;  ///< False if restores worked but entries could not be removed.

    bool all_ok() const;
  };

  /// The handlers this build knows (Phase 0: display_topology).
  std::vector<std::unique_ptr<state_handler_t>> default_handlers();

  /**
   * @brief Restores every journaled change, newest first, and removes each entry once restored.
   *
   * Entries whose handler fails stay in the journal so a later attempt (next service start, hotkey,
   * recover.exe) can retry.
   */
  result_t<restore_report_t> restore_all(state_journal_t &journal, std::vector<std::unique_ptr<state_handler_t>> &handlers, const restore_options_t &options);

  const char *outcome_name(restore_outcome_e o);

  /**
   * @brief Journals and applies a display topology change (for blanking, virtual displays, tests).
   *
   * Captures the current topology as `before`, journals it, applies `wanted`, and marks it applied.
   * If applying fails, or leaves no active display, the previous topology is put back immediately and
   * the entry removed. Returns the entry id.
   */
  result_t<std::string> journaled_display_change(state_journal_t &journal, std::string_view owner, const display::topology_t &wanted);

}  // namespace tessera::state
