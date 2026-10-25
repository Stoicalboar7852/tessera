/**
 * @file host-helpers/common/src/state_journal.cpp
 * @brief Implementation of the write-ahead state journal.
 */
// local includes
#include "tessera/state_journal.h"
#include "tessera/crypto.h"
#include "tessera/win32.h"

// standard includes
#include <algorithm>
#include <optional>
#include <thread>

// platform includes
#include <aclapi.h>
#include <sddl.h>

namespace fs = std::filesystem;
using nlohmann::json;

namespace tessera::state {

  namespace {

    error_t invalid(std::string message) {
      return make_error(tessera_code_e::invalid_data, "state_journal", std::move(message));
    }

    const char *state_name(entry_state_e s) {
      return s == entry_state_e::applied ? "applied" : "pending";
    }

    /// Canonical dump: sorted keys (nlohmann default), no whitespace, invalid UTF-8 replaced instead of throwing.
    std::string canonical_dump(const json &j) {
      return j.dump(-1, ' ', false, json::error_handler_t::replace);
    }

    json entry_to_json(const journal_entry_t &e) {
      return json {
        {"id", e.id},
        {"kind", e.kind},
        {"owner", e.owner},
        {"created_unix_ms", e.created_unix_ms},
        {"state", state_name(e.state)},
        {"before", e.before},
        {"after", e.after},
      };
    }

    bool is_entry_id(std::string_view s) {
      return s.size() == 32 && std::all_of(s.begin(), s.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
             });
    }

    bool is_kind(std::string_view s) {
      return !s.empty() && s.size() <= 64 && std::all_of(s.begin(), s.end(), [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
             });
    }

    bool is_owner(std::string_view s) {
      return s.size() <= 128 && std::all_of(s.begin(), s.end(), [](char c) {
               const auto u = static_cast<unsigned char>(c);
               return u >= 0x20 && u != 0x7f;
             });
    }

    std::int64_t now_unix_ms() {
      using namespace std::chrono;
      return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }

    fs::path with_suffix(const fs::path &p, const wchar_t *suffix) {
      fs::path out = p;
      out += suffix;
      return out;
    }

    /// Reads a whole file. Returns nullopt if it does not exist.
    result_t<std::optional<std::string>> read_file(const fs::path &p) {
      win32::unique_handle_t h(CreateFileW(
        p.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,  // never block a writer's replace
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
      ));
      if (!h) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
          return std::optional<std::string> {};
        }
        return win32::win32_error(err, "CreateFileW(read)", win32::to_utf8(p.native()));
      }
      LARGE_INTEGER size {};
      if (!GetFileSizeEx(h.get(), &size)) {
        return win32::last_error("GetFileSizeEx", win32::to_utf8(p.native()));
      }
      if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > max_file_bytes) {
        return make_error(tessera_code_e::too_large, "state_journal", "file exceeds 1 MiB: " + win32::to_utf8(p.native()));
      }
      std::string data(static_cast<std::size_t>(size.QuadPart), '\0');
      std::size_t done = 0;
      while (done < data.size()) {
        DWORD got = 0;
        if (!ReadFile(h.get(), data.data() + done, static_cast<DWORD>(data.size() - done), &got, nullptr)) {
          return win32::last_error("ReadFile", win32::to_utf8(p.native()));
        }
        if (got == 0) {
          data.resize(done);  // file shrank while reading
          break;
        }
        done += got;
      }
      return std::optional<std::string> {std::move(data)};
    }

    /// Writes `data` to `p` (replacing it) and flushes it to disk before returning.
    status_t write_file_durable(const fs::path &p, std::string_view data) {
      win32::unique_handle_t h(CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
      if (!h) {
        return win32::last_error("CreateFileW(write)", win32::to_utf8(p.native()));
      }
      std::size_t done = 0;
      while (done < data.size()) {
        DWORD wrote = 0;
        if (!WriteFile(h.get(), data.data() + done, static_cast<DWORD>(data.size() - done), &wrote, nullptr)) {
          return win32::last_error("WriteFile", win32::to_utf8(p.native()));
        }
        done += wrote;
      }
      if (!FlushFileBuffers(h.get())) {
        return win32::last_error("FlushFileBuffers", win32::to_utf8(p.native()));
      }
      return status_t::ok();
    }

    bool exists_no_throw(const fs::path &p) {
      std::error_code ec;
      return fs::exists(p, ec);
    }

    /// Moves `tmp` over `target`, keeping the previous `target` as `bak`.
    status_t replace_atomically(const fs::path &target, const fs::path &tmp, const fs::path &bak) {
      if (!exists_no_throw(target)) {
        if (!MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
          return win32::last_error("MoveFileExW", win32::to_utf8(target.native()));
        }
        return status_t::ok();
      }
      if (ReplaceFileW(target.c_str(), tmp.c_str(), bak.c_str(), REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS, nullptr, nullptr)) {
        return status_t::ok();
      }
      const DWORD err = GetLastError();
      if (err == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2) {
        // target was already renamed to the backup name; finish by moving the replacement into place.
        if (!MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
          return win32::last_error("MoveFileExW(after ReplaceFileW)", win32::to_utf8(target.native()));
        }
        return status_t::ok();
      }
      if (err == ERROR_UNABLE_TO_MOVE_REPLACEMENT || err == ERROR_UNABLE_TO_REMOVE_REPLACED) {
        // target is unchanged; fall back to copy-to-backup then move.
        if (!CopyFileW(target.c_str(), bak.c_str(), FALSE)) {
          return win32::last_error("CopyFileW(backup)", win32::to_utf8(bak.native()));
        }
        if (!MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
          return win32::last_error("MoveFileExW(fallback)", win32::to_utf8(target.native()));
        }
        return status_t::ok();
      }
      return win32::win32_error(err, "ReplaceFileW", win32::to_utf8(target.native()));
    }

    /// Retries transient sharing failures (antivirus scanners briefly open new files).
    template<class Fn>
    status_t with_retries(Fn &&fn) {
      status_t st;
      for (int attempt = 0; attempt < 6; ++attempt) {
        st = fn();
        if (st || st.error().domain != error_t::domain_e::win32 ||
            (st.error().code != ERROR_SHARING_VIOLATION && st.error().code != ERROR_ACCESS_DENIED && st.error().code != ERROR_LOCK_VIOLATION)) {
          return st;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20 << attempt));
      }
      return st;
    }

    /// Exclusive inter-process lock: an open handle on `<journal>.lock` with no sharing.
    class file_lock_t {
    public:
      static result_t<file_lock_t> acquire(const fs::path &p, std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
          HANDLE h = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
          if (h != INVALID_HANDLE_VALUE) {
            return file_lock_t(win32::unique_handle_t(h));
          }
          const DWORD err = GetLastError();
          if (err != ERROR_SHARING_VIOLATION) {
            return win32::win32_error(err, "CreateFileW(lock)", win32::to_utf8(p.native()));
          }
          if (std::chrono::steady_clock::now() >= deadline) {
            return make_error(tessera_code_e::timeout, "state_journal", "timed out waiting for journal lock " + win32::to_utf8(p.native()));
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
      }

    private:
      explicit file_lock_t(win32::unique_handle_t h):
          h_(std::move(h)) {}

      win32::unique_handle_t h_;
    };

    /// Reads main, else backup, else empty. Does not lock (writes are replace-based, so readers see a whole file).
    result_t<journal_snapshot_t> read_journal(const fs::path &main, const fs::path &bak) {
      journal_snapshot_t snap;
      auto main_text = read_file(main);
      if (main_text && main_text.value().has_value()) {
        auto doc = parse_document(*main_text.value());
        if (doc) {
          snap.doc = std::move(doc.value());
          return snap;
        }
        snap.warnings.push_back("main journal rejected: " + doc.error().to_string());
      } else if (!main_text) {
        snap.warnings.push_back("main journal unreadable: " + main_text.error().to_string());
      }
      const bool main_present = !main_text || main_text.value().has_value();

      auto bak_text = read_file(bak);
      if (bak_text && bak_text.value().has_value()) {
        auto doc = parse_document(*bak_text.value());
        if (doc) {
          snap.doc = std::move(doc.value());
          snap.recovered_from_backup = true;
          if (!main_present) {
            snap.warnings.push_back("main journal missing; using backup");
          }
          return snap;
        }
        snap.warnings.push_back("backup journal rejected: " + doc.error().to_string());
      } else if (!bak_text) {
        snap.warnings.push_back("backup journal unreadable: " + bak_text.error().to_string());
      }

      if (!main_present && bak_text && !bak_text.value().has_value()) {
        return snap;  // first use: no journal yet
      }
      std::string all;
      for (const auto &w : snap.warnings) {
        all += (all.empty() ? "" : "; ") + w;
      }
      return invalid("journal unreadable, refusing to guess: " + all);
    }

  }  // namespace

  status_t validate_entry(const journal_entry_t &e) {
    if (!is_entry_id(e.id)) {
      return invalid("entry id must be 32 lowercase hex characters");
    }
    if (!is_kind(e.kind)) {
      return invalid("entry kind must match [a-z0-9_]{1,64}: '" + e.kind.substr(0, 80) + "'");
    }
    if (!is_owner(e.owner)) {
      return invalid("entry owner must be at most 128 printable bytes");
    }
    if (e.created_unix_ms < 0) {
      return invalid("entry created_unix_ms must not be negative");
    }
    if (!e.before.is_object()) {
      return invalid("entry before must be a JSON object (entry " + e.id + ")");
    }
    if (!e.after.is_object() && !e.after.is_null()) {
      return invalid("entry after must be a JSON object or null (entry " + e.id + ")");
    }
    return status_t::ok();
  }

  result_t<std::string> serialize_document(const journal_document_t &doc) {
    if (doc.entries.size() > max_entries) {
      return make_error(tessera_code_e::too_large, "state_journal", "more than 256 entries");
    }
    json entries = json::array();
    for (const auto &e : doc.entries) {
      if (auto st = validate_entry(e); !st) {
        return st.error();
      }
      entries.push_back(entry_to_json(e));
    }
    auto sha = crypto::sha256_hex(canonical_dump(entries));
    if (!sha) {
      return sha.error();
    }
    json root {
      {"format", journal_format},
      {"version", journal_version},
      {"generation", doc.generation},
      {"entries_sha256", sha.value()},
      {"entries", std::move(entries)},
    };
    std::string text = root.dump(2, ' ', false, json::error_handler_t::replace);
    text.push_back('\n');
    if (text.size() > max_file_bytes) {
      return make_error(tessera_code_e::too_large, "state_journal", "serialised journal exceeds 1 MiB");
    }
    return text;
  }

  result_t<journal_document_t> parse_document(std::string_view text) {
    if (text.size() > max_file_bytes) {
      return make_error(tessera_code_e::too_large, "state_journal", "journal exceeds 1 MiB");
    }
    try {
      const json root = json::parse(text.begin(), text.end(), nullptr, false);
      if (root.is_discarded() || !root.is_object()) {
        return invalid("not a JSON object");
      }
      const auto fmt = root.find("format");
      if (fmt == root.end() || !fmt->is_string() || fmt->get<std::string>() != journal_format) {
        return invalid("format is not '" + std::string(journal_format) + "'");
      }
      const auto ver = root.find("version");
      if (ver == root.end() || !ver->is_number_integer()) {
        return invalid("version missing");
      }
      if (ver->get<std::int64_t>() != journal_version) {
        return make_error(tessera_code_e::unsupported, "state_journal", "journal version " + std::to_string(ver->get<std::int64_t>()) + " is not supported by this build");
      }
      const auto gen = root.find("generation");
      if (gen == root.end() || !gen->is_number_unsigned()) {
        return invalid("generation missing or negative");
      }
      const auto entries = root.find("entries");
      if (entries == root.end() || !entries->is_array()) {
        return invalid("entries missing");
      }
      if (entries->size() > max_entries) {
        return make_error(tessera_code_e::too_large, "state_journal", "more than 256 entries");
      }
      const auto sha_field = root.find("entries_sha256");
      if (sha_field == root.end() || !sha_field->is_string()) {
        return invalid("entries_sha256 missing");
      }
      auto sha = crypto::sha256_hex(canonical_dump(*entries));
      if (!sha) {
        return sha.error();
      }
      if (sha.value() != sha_field->get<std::string>()) {
        return invalid("entries checksum mismatch (file damaged or edited)");
      }

      journal_document_t doc;
      doc.generation = gen->get<std::uint64_t>();
      for (const auto &je : *entries) {
        if (!je.is_object()) {
          return invalid("entry is not an object");
        }
        auto str = [&](const char *key) -> std::optional<std::string> {
          const auto it = je.find(key);
          if (it == je.end() || !it->is_string()) {
            return std::nullopt;
          }
          return it->get<std::string>();
        };
        journal_entry_t e;
        auto id = str("id");
        auto kind = str("kind");
        auto owner = str("owner");
        auto state = str("state");
        const auto created = je.find("created_unix_ms");
        if (!id || !kind || !owner || !state || created == je.end() || !created->is_number_integer()) {
          return invalid("entry is missing a required field");
        }
        e.id = std::move(*id);
        e.kind = std::move(*kind);
        e.owner = std::move(*owner);
        e.created_unix_ms = created->get<std::int64_t>();
        if (*state == "pending") {
          e.state = entry_state_e::pending;
        } else if (*state == "applied") {
          e.state = entry_state_e::applied;
        } else {
          return invalid("entry state must be 'pending' or 'applied'");
        }
        e.before = je.value("before", json());
        e.after = je.value("after", json());
        if (auto st = validate_entry(e); !st) {
          return st.error();
        }
        const bool duplicate = std::any_of(doc.entries.begin(), doc.entries.end(), [&](const journal_entry_t &x) {
          return x.id == e.id;
        });
        if (duplicate) {
          return invalid("duplicate entry id " + e.id);
        }
        doc.entries.push_back(std::move(e));
      }
      return doc;
    } catch (const std::exception &ex) {
      return invalid(std::string("exception while parsing: ") + ex.what());
    }
  }

  result_t<std::string> new_entry_id() {
    std::uint8_t raw[16];
    if (auto st = crypto::random_bytes(raw, sizeof(raw)); !st) {
      return st.error();
    }
    return crypto::to_hex(raw, sizeof(raw));
  }

  status_t secure_journal_directory(const fs::path &dir) {
    // Protected DACL: SYSTEM and Administrators full control, Users read and execute; inherited by children.
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FRFX;;;BU)", SDDL_REVISION_1, &sd, nullptr)) {
      return win32::last_error("ConvertStringSecurityDescriptorToSecurityDescriptorW");
    }
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL dacl = nullptr;
    status_t result;
    if (!GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted)) {
      result = win32::last_error("GetSecurityDescriptorDacl");
    } else {
      std::wstring path = dir.native();
      const DWORD err = SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr);
      if (err != ERROR_SUCCESS) {
        result = win32::win32_error(err, "SetNamedSecurityInfoW", win32::to_utf8(path));
      }
    }
    LocalFree(sd);
    return result;
  }

  state_journal_t::state_journal_t(options_t options):
      options_(std::move(options)) {}

  result_t<fs::path> state_journal_t::default_path() {
    auto base = win32::program_data_dir();
    if (!base) {
      return base.error();
    }
    return base.value() / L"Tessera" / L"state-journal.json";
  }

  fs::path state_journal_t::backup_path() const {
    return with_suffix(options_.file, L".bak");
  }

  fs::path state_journal_t::temp_path() const {
    return with_suffix(options_.file, L".tmp");
  }

  fs::path state_journal_t::lock_path() const {
    return with_suffix(options_.file, L".lock");
  }

  result_t<journal_snapshot_t> state_journal_t::read() const {
    return read_journal(options_.file, backup_path());
  }

  template<class Fn>
  status_t state_journal_t::modify(std::string_view where, Fn &&fn) {
    std::error_code ec;
    fs::create_directories(options_.file.parent_path(), ec);
    if (ec) {
      return make_error(tessera_code_e::invalid_data, std::string(where), "cannot create journal directory: " + ec.message());
    }
    auto lock = file_lock_t::acquire(lock_path(), options_.lock_timeout);
    if (!lock) {
      return lock.error();
    }
    auto snap = read_journal(options_.file, backup_path());
    if (!snap) {
      return snap.error();  // never overwrite a journal we cannot read
    }
    journal_document_t doc = std::move(snap.value().doc);
    if (status_t st = fn(doc); !st) {
      return st;
    }
    doc.generation += 1;
    auto text = serialize_document(doc);
    if (!text) {
      return text.error();
    }
    if (snap.value().recovered_from_backup && exists_no_throw(options_.file)) {
      // Keep the damaged main file for diagnosis, and keep the good backup as the backup.
      const fs::path quarantine = with_suffix(options_.file, (L".corrupt-" + std::to_wstring(now_unix_ms())).c_str());
      if (!MoveFileExW(options_.file.c_str(), quarantine.c_str(), MOVEFILE_WRITE_THROUGH)) {
        return win32::last_error("MoveFileExW(quarantine)", win32::to_utf8(options_.file.native()));
      }
    }
    if (auto st = with_retries([&] { return write_file_durable(temp_path(), text.value()); }); !st) {
      return st;
    }
    return with_retries([&] { return replace_atomically(options_.file, temp_path(), backup_path()); });
  }

  result_t<std::string> state_journal_t::begin(std::string_view kind, std::string_view owner, json before) {
    auto id = new_entry_id();
    if (!id) {
      return id.error();
    }
    journal_entry_t entry;
    entry.id = id.value();
    entry.kind = std::string(kind);
    entry.owner = std::string(owner);
    entry.created_unix_ms = now_unix_ms();
    entry.state = entry_state_e::pending;
    entry.before = std::move(before);
    if (auto st = validate_entry(entry); !st) {
      return st.error();
    }
    auto st = modify("state_journal_t::begin", [&](journal_document_t &doc) -> status_t {
      if (doc.entries.size() >= max_entries) {
        return make_error(tessera_code_e::too_large, "state_journal_t::begin", "journal already holds 256 entries");
      }
      doc.entries.push_back(std::move(entry));
      return status_t::ok();
    });
    if (!st) {
      return st.error();
    }
    return id;
  }

  status_t state_journal_t::mark_applied(std::string_view id, json after) {
    if (!after.is_object() && !after.is_null()) {
      return invalid("after must be a JSON object or null");
    }
    return modify("state_journal_t::mark_applied", [&](journal_document_t &doc) -> status_t {
      for (auto &e : doc.entries) {
        if (e.id == id) {
          e.state = entry_state_e::applied;
          e.after = std::move(after);
          return status_t::ok();
        }
      }
      return make_error(tessera_code_e::not_found, "state_journal_t::mark_applied", "no entry " + std::string(id));
    });
  }

  status_t state_journal_t::remove(std::string_view id) {
    return modify("state_journal_t::remove", [&](journal_document_t &doc) -> status_t {
      const auto it = std::find_if(doc.entries.begin(), doc.entries.end(), [&](const journal_entry_t &e) {
        return e.id == id;
      });
      if (it == doc.entries.end()) {
        return make_error(tessera_code_e::not_found, "state_journal_t::remove", "no entry " + std::string(id));
      }
      doc.entries.erase(it);
      return status_t::ok();
    });
  }

}  // namespace tessera::state
