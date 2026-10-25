/**
 * @file host-helpers/tests/state_journal_test.cpp
 * @brief Unit tests for the write-ahead state journal.
 */
// standard includes
#include <atomic>
#include <fstream>
#include <set>
#include <thread>

// lib includes
#include <gtest/gtest.h>

// local includes
#include "tessera/crypto.h"
#include "tessera/state_journal.h"
#include "tessera/win32.h"

namespace fs = std::filesystem;
using nlohmann::json;
using namespace tessera;
using namespace tessera::state;

namespace {

  class StateJournalTest: public ::testing::Test {
  protected:
    void SetUp() override {
      auto id = new_entry_id();
      ASSERT_TRUE(id) << id.error().to_string();
      dir_ = fs::temp_directory_path() / "tessera-tests" / id.value();
      fs::create_directories(dir_);
      file_ = dir_ / "state-journal.json";
    }

    void TearDown() override {
      std::error_code ec;
      fs::remove_all(dir_, ec);
    }

    state_journal_t journal(std::chrono::milliseconds lock_timeout = std::chrono::milliseconds(5000)) const {
      return state_journal_t({file_, lock_timeout});
    }

    static void write_text(const fs::path &p, std::string_view text) {
      std::ofstream out(p, std::ios::binary | std::ios::trunc);
      out.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    static std::string read_text(const fs::path &p) {
      std::ifstream in(p, std::ios::binary);
      return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    static journal_entry_t sample_entry(std::string id) {
      journal_entry_t e;
      e.id = std::move(id);
      e.kind = "display_topology";
      e.owner = "test";
      e.created_unix_ms = 1761400000000;
      e.state = entry_state_e::pending;
      e.before = json {{"paths", json::array({1, 2, 3})}, {"refresh", 59.94}};
      return e;
    }

    fs::path dir_;
    fs::path file_;
  };

  const std::string id_a = "0123456789abcdef0123456789abcdef";
  const std::string id_b = "fedcba9876543210fedcba9876543210";

}  // namespace

TEST(StateJournalPure, NewEntryIdIs32LowercaseHex) {
  std::set<std::string> seen;
  for (int i = 0; i < 100; ++i) {
    auto id = new_entry_id();
    ASSERT_TRUE(id);
    ASSERT_EQ(id.value().size(), 32u);
    for (char c : id.value()) {
      ASSERT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << id.value();
    }
    seen.insert(id.value());
  }
  EXPECT_EQ(seen.size(), 100u);
}

TEST_F(StateJournalTest, SerializeParseRoundTrip) {
  journal_document_t doc;
  doc.generation = 7;
  doc.entries.push_back(sample_entry(id_a));
  auto applied = sample_entry(id_b);
  applied.state = entry_state_e::applied;
  applied.after = json {{"active", false}};
  doc.entries.push_back(applied);

  auto text = serialize_document(doc);
  ASSERT_TRUE(text) << text.error().to_string();
  auto parsed = parse_document(text.value());
  ASSERT_TRUE(parsed) << parsed.error().to_string();
  EXPECT_EQ(parsed->generation, 7u);
  ASSERT_EQ(parsed->entries.size(), 2u);
  EXPECT_EQ(parsed->entries[0].id, id_a);
  EXPECT_EQ(parsed->entries[0].state, entry_state_e::pending);
  EXPECT_EQ(parsed->entries[0].before, doc.entries[0].before);
  EXPECT_TRUE(parsed->entries[0].after.is_null());
  EXPECT_EQ(parsed->entries[1].state, entry_state_e::applied);
  EXPECT_EQ(parsed->entries[1].after, applied.after);
}

TEST_F(StateJournalTest, ParseRejectsEditedContent) {
  journal_document_t doc;
  doc.entries.push_back(sample_entry(id_a));
  auto text = serialize_document(doc).value();
  const auto pos = text.find("\"test\"");
  ASSERT_NE(pos, std::string::npos);
  text.replace(pos, 6, "\"evil\"");
  auto parsed = parse_document(text);
  ASSERT_FALSE(parsed);
  EXPECT_EQ(parsed.error().code, static_cast<std::uint32_t>(tessera_code_e::invalid_data));
  EXPECT_NE(parsed.error().message.find("checksum"), std::string::npos);
}

TEST_F(StateJournalTest, ParseRejectsGarbageAndTruncation) {
  EXPECT_FALSE(parse_document(""));
  EXPECT_FALSE(parse_document("\x00\x01\x02 not json"));
  EXPECT_FALSE(parse_document("[]"));
  journal_document_t doc;
  doc.entries.push_back(sample_entry(id_a));
  const auto text = serialize_document(doc).value();
  for (std::size_t cut : {std::size_t {1}, text.size() / 2, text.size() - 3}) {
    EXPECT_FALSE(parse_document(std::string_view(text).substr(0, cut))) << "cut at " << cut;
  }
}

TEST_F(StateJournalTest, ParseRejectsWrongFormatAndVersion) {
  journal_document_t doc;
  auto j = json::parse(serialize_document(doc).value());
  j["format"] = "something-else";
  EXPECT_FALSE(parse_document(j.dump()));

  j = json::parse(serialize_document(doc).value());
  j["version"] = 2;
  auto parsed = parse_document(j.dump());
  ASSERT_FALSE(parsed);
  EXPECT_EQ(parsed.error().code, static_cast<std::uint32_t>(tessera_code_e::unsupported));

  j = json::parse(serialize_document(doc).value());
  j["generation"] = -1;
  EXPECT_FALSE(parse_document(j.dump()));
}

TEST_F(StateJournalTest, ParseRejectsOversizedInput) {
  const std::string big(max_file_bytes + 1, ' ');
  auto parsed = parse_document(big);
  ASSERT_FALSE(parsed);
  EXPECT_EQ(parsed.error().code, static_cast<std::uint32_t>(tessera_code_e::too_large));
}

TEST_F(StateJournalTest, ValidateEntryRejectsBadFields) {
  auto e = sample_entry(id_a);
  EXPECT_TRUE(validate_entry(e));

  auto bad = e;
  bad.id = "ABCDEF0123456789ABCDEF0123456789";  // uppercase
  EXPECT_FALSE(validate_entry(bad));
  bad = e;
  bad.id = "short";
  EXPECT_FALSE(validate_entry(bad));
  bad = e;
  bad.kind = "Display-Topology";
  EXPECT_FALSE(validate_entry(bad));
  bad = e;
  bad.kind = std::string(65, 'a');
  EXPECT_FALSE(validate_entry(bad));
  bad = e;
  bad.owner = std::string(129, 'x');
  EXPECT_FALSE(validate_entry(bad));
  bad = e;
  bad.owner = "line\nbreak";
  EXPECT_FALSE(validate_entry(bad));
  bad = e;
  bad.before = json::array();
  EXPECT_FALSE(validate_entry(bad));
  bad = e;
  bad.after = 42;
  EXPECT_FALSE(validate_entry(bad));
  bad = e;
  bad.created_unix_ms = -5;
  EXPECT_FALSE(validate_entry(bad));
}

TEST_F(StateJournalTest, ParseRejectsDuplicateIds) {
  journal_document_t doc;
  doc.entries.push_back(sample_entry(id_a));
  auto j = json::parse(serialize_document(doc).value());
  j["entries"].push_back(j["entries"][0]);
  // Give the tampered document a valid checksum, so only the duplicate check can reject it.
  j["entries_sha256"] = crypto::sha256_hex(j["entries"].dump()).value();
  auto parsed = parse_document(j.dump());
  ASSERT_FALSE(parsed);
  EXPECT_NE(parsed.error().message.find("duplicate"), std::string::npos) << parsed.error().to_string();
}

TEST_F(StateJournalTest, EmptyWhenNoFiles) {
  auto snap = journal().read();
  ASSERT_TRUE(snap) << snap.error().to_string();
  EXPECT_EQ(snap->doc.generation, 0u);
  EXPECT_TRUE(snap->doc.entries.empty());
  EXPECT_FALSE(snap->recovered_from_backup);
}

TEST_F(StateJournalTest, BeginMarkAppliedRemove) {
  auto j = journal();
  auto id = j.begin("display_topology", "session:1", json {{"x", 1}});
  ASSERT_TRUE(id) << id.error().to_string();

  auto snap = j.read();
  ASSERT_TRUE(snap);
  ASSERT_EQ(snap->doc.entries.size(), 1u);
  EXPECT_EQ(snap->doc.entries[0].state, entry_state_e::pending);
  EXPECT_EQ(snap->doc.entries[0].owner, "session:1");
  EXPECT_EQ(snap->doc.generation, 1u);

  ASSERT_TRUE(j.mark_applied(id.value(), json {{"x", 2}}));
  snap = j.read();
  ASSERT_TRUE(snap);
  EXPECT_EQ(snap->doc.entries[0].state, entry_state_e::applied);
  EXPECT_EQ(snap->doc.entries[0].after, (json {{"x", 2}}));
  EXPECT_EQ(snap->doc.generation, 2u);

  ASSERT_TRUE(j.remove(id.value()));
  snap = j.read();
  ASSERT_TRUE(snap);
  EXPECT_TRUE(snap->doc.entries.empty());
  EXPECT_EQ(snap->doc.generation, 3u);

  EXPECT_TRUE(fs::exists(j.backup_path()));
  EXPECT_FALSE(fs::exists(j.temp_path()));
}

TEST_F(StateJournalTest, UnknownIdIsNotFound) {
  auto j = journal();
  auto st = j.mark_applied(id_a, json::object());
  ASSERT_FALSE(st);
  EXPECT_EQ(st.error().code, static_cast<std::uint32_t>(tessera_code_e::not_found));
  st = j.remove(id_a);
  ASSERT_FALSE(st);
  EXPECT_EQ(st.error().code, static_cast<std::uint32_t>(tessera_code_e::not_found));
}

TEST_F(StateJournalTest, RejectsInvalidBeginArguments) {
  auto j = journal();
  EXPECT_FALSE(j.begin("Bad Kind", "test", json::object()));
  EXPECT_FALSE(j.begin("display_topology", "test", json::array()));
  EXPECT_FALSE(fs::exists(file_)) << "nothing may be written for an invalid entry";
}

TEST_F(StateJournalTest, FallsBackToBackupWhenMainIsCorrupt) {
  auto j = journal();
  ASSERT_TRUE(j.begin("display_topology", "a", json::object()));  // gen 1 -> main
  ASSERT_TRUE(j.begin("display_topology", "b", json::object()));  // gen 2 -> main, gen 1 -> bak
  const std::string main_text = read_text(file_);
  write_text(file_, main_text.substr(0, main_text.size() / 2));  // simulate a damaged main file

  auto snap = j.read();
  ASSERT_TRUE(snap) << snap.error().to_string();
  EXPECT_TRUE(snap->recovered_from_backup);
  EXPECT_EQ(snap->doc.generation, 1u);
  ASSERT_EQ(snap->doc.entries.size(), 1u);
  EXPECT_EQ(snap->doc.entries[0].owner, "a");
  EXPECT_FALSE(snap->warnings.empty());
}

TEST_F(StateJournalTest, WriteAfterCorruptMainKeepsGoodBackupAndQuarantinesDamage) {
  auto j = journal();
  ASSERT_TRUE(j.begin("display_topology", "a", json::object()));
  ASSERT_TRUE(j.begin("display_topology", "b", json::object()));
  write_text(file_, "{ damaged");
  const std::string good_backup = read_text(j.backup_path());

  ASSERT_TRUE(j.begin("display_topology", "c", json::object()));

  EXPECT_EQ(read_text(j.backup_path()), good_backup) << "the good backup must not be replaced by the damaged file";
  bool quarantined = false;
  for (const auto &f : fs::directory_iterator(dir_)) {
    quarantined |= f.path().filename().wstring().find(L".corrupt-") != std::wstring::npos;
  }
  EXPECT_TRUE(quarantined);

  auto snap = j.read();
  ASSERT_TRUE(snap);
  EXPECT_FALSE(snap->recovered_from_backup);
  ASSERT_EQ(snap->doc.entries.size(), 2u);  // a (from backup) + c
  EXPECT_EQ(snap->doc.entries[1].owner, "c");
}

TEST_F(StateJournalTest, BothCorruptIsAnErrorAndNothingIsOverwritten) {
  auto j = journal();
  ASSERT_TRUE(j.begin("display_topology", "a", json::object()));
  ASSERT_TRUE(j.begin("display_topology", "b", json::object()));
  write_text(file_, "garbage-main");
  write_text(j.backup_path(), "garbage-bak");

  EXPECT_FALSE(j.read());
  EXPECT_FALSE(j.begin("display_topology", "c", json::object()));
  EXPECT_EQ(read_text(file_), "garbage-main");
  EXPECT_EQ(read_text(j.backup_path()), "garbage-bak");
}

TEST_F(StateJournalTest, MainMissingUsesBackup) {
  auto j = journal();
  ASSERT_TRUE(j.begin("display_topology", "a", json::object()));
  ASSERT_TRUE(j.begin("display_topology", "b", json::object()));
  fs::remove(file_);
  auto snap = j.read();
  ASSERT_TRUE(snap);
  EXPECT_TRUE(snap->recovered_from_backup);
  EXPECT_EQ(snap->doc.entries.size(), 1u);
}

TEST_F(StateJournalTest, StaleTempFileFromCrashIsIgnored) {
  auto j = journal();
  write_text(j.temp_path(), "half-written");
  auto snap = j.read();
  ASSERT_TRUE(snap);
  EXPECT_TRUE(snap->doc.entries.empty());
  ASSERT_TRUE(j.begin("display_topology", "a", json::object()));
  EXPECT_FALSE(fs::exists(j.temp_path()));
}

TEST_F(StateJournalTest, LockTimeoutWhenAnotherWriterHoldsTheLock) {
  auto j = journal(std::chrono::milliseconds(150));
  win32::unique_handle_t holder(CreateFileW(j.lock_path().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
  ASSERT_TRUE(holder);
  auto id = j.begin("display_topology", "a", json::object());
  ASSERT_FALSE(id);
  EXPECT_EQ(id.error().code, static_cast<std::uint32_t>(tessera_code_e::timeout));
  holder.reset();
  EXPECT_TRUE(j.begin("display_topology", "a", json::object()));
}

TEST_F(StateJournalTest, EntryLimitIsEnforced) {
  auto j = journal();
  journal_document_t doc;
  doc.generation = 1;
  for (std::size_t i = 0; i < max_entries; ++i) {
    doc.entries.push_back(sample_entry(new_entry_id().value()));
  }
  auto text = serialize_document(doc);
  ASSERT_TRUE(text) << text.error().to_string();
  write_text(file_, text.value());

  auto id = j.begin("display_topology", "overflow", json::object());
  ASSERT_FALSE(id);
  EXPECT_EQ(id.error().code, static_cast<std::uint32_t>(tessera_code_e::too_large));
}

TEST_F(StateJournalTest, ConcurrentWritersDoNotLoseEntries) {
  constexpr int threads = 4;
  constexpr int per_thread = 15;
  std::atomic<int> failures {0};
  std::vector<std::thread> pool;
  for (int t = 0; t < threads; ++t) {
    pool.emplace_back([&, t] {
      auto j = journal(std::chrono::milliseconds(20000));
      for (int i = 0; i < per_thread; ++i) {
        if (!j.begin("display_topology", "thread:" + std::to_string(t), json {{"i", i}})) {
          ++failures;
        }
      }
    });
  }
  for (auto &th : pool) {
    th.join();
  }
  EXPECT_EQ(failures.load(), 0);
  auto snap = journal().read();
  ASSERT_TRUE(snap);
  EXPECT_EQ(snap->doc.entries.size(), static_cast<std::size_t>(threads * per_thread));
  EXPECT_EQ(snap->doc.generation, static_cast<std::uint64_t>(threads * per_thread));
}

TEST_F(StateJournalTest, InvalidUtf8InPayloadDoesNotThrow) {
  auto j = journal();
  json before = {{"name", std::string("bad\xff\xfe utf8")}};
  auto id = j.begin("display_topology", "test", before);
  ASSERT_TRUE(id) << id.error().to_string();
  auto snap = j.read();
  ASSERT_TRUE(snap) << snap.error().to_string();
  EXPECT_EQ(snap->doc.entries.size(), 1u);
}
