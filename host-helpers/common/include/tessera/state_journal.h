/**
 * @file host-helpers/common/include/tessera/state_journal.h
 * @brief Write-ahead journal of host system-state changes, so every change can be undone after a
 *        disconnect, crash, reboot or emergency hotkey (brief §4.2).
 *
 * Protocol for a change:
 *   1. `begin(kind, owner, before)` records the previous state as a *pending* entry and persists it.
 *   2. The caller changes the system.
 *   3. `mark_applied(id, after)` records that the change happened.
 *   4. On restore, a handler for `kind` puts `before` back, then `remove(id)` drops the entry.
 * A pending entry means "the change may or may not have happened": handlers must restore idempotently.
 *
 * File format (UTF-8 JSON, at most `max_file_bytes`):
 *   { "format": "tessera-state-journal", "version": 1, "generation": N,
 *     "entries_sha256": "<hex of SHA-256 over the canonical dump of entries>",
 *     "entries": [ { "id", "kind", "owner", "created_unix_ms", "state", "before", "after" }, ... ] }
 *
 * Every write goes to `<file>.tmp`, is flushed, then atomically replaces `<file>`; the previous file
 * becomes `<file>.bak`. A reader that finds `<file>` missing or corrupt falls back to `<file>.bak`.
 * Writers in different processes serialise through an exclusive lock on `<file>.lock`.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "tessera/result.h"

namespace tessera::state {

  inline constexpr std::string_view journal_format = "tessera-state-journal";
  inline constexpr int journal_version = 1;
  inline constexpr std::size_t max_file_bytes = 1024 * 1024;
  inline constexpr std::size_t max_entries = 256;

  enum class entry_state_e {
    pending,  ///< `before` recorded; the change may be partly or fully applied.
    applied,  ///< The change completed; `after` describes the new state.
  };

  struct journal_entry_t {
    std::string id;  ///< 32 lowercase hex characters.
    std::string kind;  ///< Handler name, `[a-z0-9_]{1,64}`, e.g. "display_topology".
    std::string owner;  ///< Who made the change, e.g. "session:3f2a..." or "recover"; at most 128 bytes.
    std::int64_t created_unix_ms = 0;
    entry_state_e state = entry_state_e::pending;
    nlohmann::json before;  ///< Handler-specific previous state (object).
    nlohmann::json after;  ///< Handler-specific new state (object or null).
  };

  struct journal_document_t {
    std::uint64_t generation = 0;
    std::vector<journal_entry_t> entries;
  };

  /// What `read()` found, including whether it had to fall back to the backup.
  struct journal_snapshot_t {
    journal_document_t doc;
    bool recovered_from_backup = false;
    std::vector<std::string> warnings;  ///< Human-readable reasons, e.g. why the main file was rejected.
  };

  /// Serialises a document (adds format, version and checksum). Pure; for tests and tools.
  result_t<std::string> serialize_document(const journal_document_t &doc);

  /// Parses and fully validates a document. Pure; for tests and tools.
  result_t<journal_document_t> parse_document(std::string_view text);

  /// Validates one entry's fields (used by parse_document and before writing).
  status_t validate_entry(const journal_entry_t &entry);

  /// Generates a new random 128-bit entry id as 32 hex characters.
  result_t<std::string> new_entry_id();

  /**
   * @brief Restricts a journal directory to SYSTEM and Administrators (full) and Users (read).
   *
   * The service acts on the journal as SYSTEM, so ordinary users must not be able to plant entries.
   * Call it from the elevated service when it creates the directory. Fails on file systems without ACLs.
   */
  status_t secure_journal_directory(const std::filesystem::path &dir);

  /**
   * @brief Process-safe access to one journal file.
   */
  class state_journal_t {
  public:
    struct options_t {
      std::filesystem::path file;  ///< Journal path; the directory is created if needed.
      std::chrono::milliseconds lock_timeout {5000};
    };

    explicit state_journal_t(options_t options);

    /// Default location: %ProgramData%\Tessera\state-journal.json.
    static result_t<std::filesystem::path> default_path();

    const std::filesystem::path &path() const {
      return options_.file;
    }

    /// Reads the journal (main file, else backup, else empty document).
    result_t<journal_snapshot_t> read() const;

    /// Records a pending change and returns its entry id.
    result_t<std::string> begin(std::string_view kind, std::string_view owner, nlohmann::json before);

    /// Marks an entry applied and stores the new state.
    status_t mark_applied(std::string_view id, nlohmann::json after);

    /// Removes an entry (after a successful restore, or when a change is abandoned before applying).
    status_t remove(std::string_view id);

    std::filesystem::path backup_path() const;
    std::filesystem::path temp_path() const;
    std::filesystem::path lock_path() const;

  private:
    template<class Fn>
    status_t modify(std::string_view where, Fn &&fn);

    options_t options_;
  };

}  // namespace tessera::state
