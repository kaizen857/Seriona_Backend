#include "seriona/scanner/song_identity.h"

#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>

namespace {

[[nodiscard]] bool isHexId(const std::string& value) {
  return value.size() == 16U && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

}

TEST_CASE("scanner song identity normalizes text for ids") {
  using seriona::scanner::normalizeForId;

  CHECK(normalizeForId("") == "");
  CHECK(normalizeForId("   \t\n") == "");
  CHECK(normalizeForId("  Title\t MIX  ") == "title mix");
}

TEST_CASE("scanner song identity content ids are stable across whitespace and case") {
  using seriona::scanner::computeContentId;

  const auto first = computeContentId(std::chrono::milliseconds{183000}, "  Song Title  ", "ARTIST NAME");
  const auto second = computeContentId(std::chrono::milliseconds{183000}, "song title", " artist name ");

  CHECK(first == second);
  CHECK(isHexId(first));
}

TEST_CASE("scanner song identity location ids handle path normalization and missing mtime") {
  using seriona::scanner::computeLocationId;

  const auto mtime = std::filesystem::file_time_type::clock::now();
  const auto first = computeLocationId("/music/../music/album/track.flac", 4096U, mtime);
  const auto second = computeLocationId("/music/album/track.flac", 4096U, mtime);
  const auto differentSize = computeLocationId("/music/album/track.flac", 4097U, mtime);
  const auto missingMtime = computeLocationId("/music/album/track.flac", 4096U, std::nullopt);

  CHECK(first == second);
  CHECK(first != differentSize);
  CHECK(first != missingMtime);
  CHECK(isHexId(first));
}

TEST_CASE("scanner song identity CUE location ids include track index") {
  using seriona::scanner::computeLocationId;

  const auto mtime = std::filesystem::file_time_type::clock::now();
  const auto offset = std::chrono::milliseconds{30000};
  const auto track0 = computeLocationId("/music/album.cue", 2048U, mtime, offset, 0U);
  const auto track1 = computeLocationId("/music/album.cue", 2048U, mtime, offset, 1U);
  const auto withoutIndex = computeLocationId("/music/album.cue", 2048U, mtime, offset);

  CHECK(track0 != track1);
  CHECK(track0 != withoutIndex);
  CHECK(track1 != withoutIndex);
  CHECK(isHexId(track0));
  CHECK(isHexId(track1));
}
