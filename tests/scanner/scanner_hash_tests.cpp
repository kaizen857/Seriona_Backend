#include "scanner_test_harness.h"

#include "seriona/scanner/directory_tree_hash.h"
#include "seriona/scanner/hash_utils.h"

#include <doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace seriona::scanner {
namespace {

void writeTextFile(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  REQUIRE(output.is_open());
  output << text;
}

void setTreeWriteTime(const std::filesystem::path& root, const std::filesystem::file_time_type time) {
  std::filesystem::last_write_time(root, time);
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    std::filesystem::last_write_time(entry.path(), time);
  }
}

[[nodiscard]] std::string requireHash(const FileHashResult& result) {
  CHECK(result.errors.empty());
  REQUIRE(result.hash.has_value());
  CHECK(result.hash->size() == 32U);
  return *result.hash;
}

[[nodiscard]] std::string requireHash(const DirectoryHashResult& result) {
  CHECK(result.errors.empty());
  REQUIRE(result.hash.has_value());
  CHECK(result.hash->size() == 32U);
  return *result.hash;
}

TEST_CASE("scanner file hash uses content not mtime") {
  test::TempScannerRoot root("scanner-hash-file");
  const auto path = root.path() / "song.flac";
  writeTextFile(path, "same content");

  const auto first = requireHash(hashFileContent(path));
  std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() - std::chrono::hours{24});
  const auto mtimeOnly = requireHash(hashFileContent(path));
  writeTextFile(path, "changed content");
  const auto changed = requireHash(hashFileContent(path));

  CHECK(first == mtimeOnly);
  CHECK(first != changed);
}

TEST_CASE("scanner sidecar lrc hash changes independently from paired audio hash") {
  test::TempScannerRoot root("scanner-hash-lrc");
  const auto audio = test::writeAudioFixture(root.path(), "song.flac");
  auto lrc = audio;
  lrc.replace_extension(".lrc");
  writeTextFile(lrc, "[00:01.00] first\n");

  const auto audioHash = requireHash(hashFileContent(audio));
  const auto lrcFirst = requireHash(hashLyricsSidecar(lrc));
  writeTextFile(lrc, "[00:01.00] changed\n");
  const auto lrcChanged = requireHash(hashLyricsSidecar(lrc));
  const auto audioAfterLrcChange = requireHash(hashFileContent(audio));

  CHECK(lrcFirst != lrcChanged);
  CHECK(audioHash == audioAfterLrcChange);
}

TEST_CASE("scanner directory merkle hash is stable and changes on child add delete and rename") {
  test::TempScannerRoot firstRoot("scanner-hash-dir-a");
  test::TempScannerRoot secondRoot("scanner-hash-dir-b");
  writeTextFile(firstRoot.path() / "b" / "two.flac", "two");
  writeTextFile(firstRoot.path() / "a" / "one.flac", "one");
  writeTextFile(secondRoot.path() / "a" / "one.flac", "one");
  writeTextFile(secondRoot.path() / "b" / "two.flac", "two");
  const auto fixedTime = std::filesystem::file_time_type::clock::now() - std::chrono::hours{48};
  setTreeWriteTime(firstRoot.path(), fixedTime);
  setTreeWriteTime(secondRoot.path(), fixedTime);

  const auto stableA = requireHash(hashDirectoryMerkle(firstRoot.path()));
  const auto stableB = requireHash(hashDirectoryMerkle(secondRoot.path()));
  CHECK(stableA == stableB);

  writeTextFile(firstRoot.path() / "c" / "three.flac", "three");
  const auto added = requireHash(hashDirectoryMerkle(firstRoot.path()));
  CHECK(added != stableA);

  std::filesystem::rename(firstRoot.path() / "c" / "three.flac", firstRoot.path() / "c" / "renamed.flac");
  const auto renamed = requireHash(hashDirectoryMerkle(firstRoot.path()));
  CHECK(renamed != added);

  std::filesystem::remove(firstRoot.path() / "c" / "renamed.flac");
  const auto deleted = requireHash(hashDirectoryMerkle(firstRoot.path()));
  CHECK(deleted != renamed);
}

TEST_CASE("scanner directory tree hash describes structure without reading file content") {
  test::TempScannerRoot root("scanner-directory-tree-hash-structure");
  writeTextFile(root.path() / "album" / "01.flac", "first audio bytes");
  writeTextFile(root.path() / "album" / "02.flac", "second audio bytes");

  const auto stable = requireHash(computeDirectoryTreeHash(root.path()));
  writeTextFile(root.path() / "album" / "01.flac", "changed audio bytes with the same tree path");
  const auto contentChanged = requireHash(computeDirectoryTreeHash(root.path()));

  CHECK(contentChanged == stable);

  writeTextFile(root.path() / "album" / "03.flac", "third audio bytes");
  const auto added = requireHash(computeDirectoryTreeHash(root.path()));
  CHECK(added != stable);

  std::filesystem::rename(root.path() / "album" / "03.flac", root.path() / "album" / "renamed.flac");
  const auto renamed = requireHash(computeDirectoryTreeHash(root.path()));
  CHECK(renamed != added);

  std::filesystem::remove(root.path() / "album" / "renamed.flac");
  const auto deleted = requireHash(computeDirectoryTreeHash(root.path()));
  CHECK(deleted != renamed);
}

TEST_CASE("scanner directory tree hash reports missing roots without partial hash") {
  test::TempScannerRoot root("scanner-directory-tree-hash-missing");
  const auto missing = root.path() / "missing";

  const auto result = computeDirectoryTreeHash(missing);

  CHECK_FALSE(result.hash.has_value());
  REQUIRE(result.errors.size() == 1U);
  CHECK(result.errors.front().code == HashErrorCode::IoFailure);
  CHECK(result.errors.front().scannerError.code == ScannerErrorCode::RootUnavailable);
  REQUIRE(result.errors.front().scannerError.path.has_value());
  CHECK(*result.errors.front().scannerError.path == missing);
}

TEST_CASE("scanner hashing reports cancellation without a partial hash") {
  test::TempScannerRoot root("scanner-hash-cancel");
  const auto path = root.path() / "song.flac";
  writeTextFile(path, std::string(1024U, 'x'));
  std::atomic_bool cancelled{true};

  const auto result = hashFileContent(path, {.chunkBytes = 16U, .cancellationRequested = &cancelled});

  CHECK_FALSE(result.hash.has_value());
  REQUIRE(result.errors.size() == 1U);
  CHECK(result.errors.front().code == HashErrorCode::Cancelled);
  CHECK(result.errors.front().scannerError.code == ScannerErrorCode::Cancelled);
}

TEST_CASE("scanner hashing maps vanished files to recoverable scanner errors") {
  test::TempScannerRoot root("scanner-hash-missing");
  const auto path = root.path() / "vanished.flac";

  const auto result = hashFileContent(path);

  CHECK_FALSE(result.hash.has_value());
  REQUIRE(result.errors.size() == 1U);
  CHECK(result.errors.front().code == HashErrorCode::IoFailure);
  CHECK(result.errors.front().scannerError.code == ScannerErrorCode::RootUnavailable);
  REQUIRE(result.errors.front().scannerError.path.has_value());
  CHECK(*result.errors.front().scannerError.path == path);
}

}
}

#include "scanner_song_identity_tests.cpp"
