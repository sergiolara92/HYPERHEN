/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>
namespace onion::sprx {
struct GameIdentity {
  pid_t pid = -1;
  int64_t seconds = 0, microseconds = 0;
  std::string title;
  intptr_t rootdir = 0, jaildir = 0;
};
bool capture_game(pid_t pid, GameIdentity &identity) noexcept;
bool same_game(const GameIdentity &identity) noexcept;
bool game_version(const GameIdentity &identity, std::string &version,
                  std::string &metadata_hash) noexcept;
struct RaiGameMetadata {
  std::string title, content, version, metadata_hash;
};
struct RaiGameSnapshot {
  int32_t pid = -1;
  std::string name, path;
  uint64_t base = 0;
};
/** Read and verify SFO before stopping the game; never re-open its sandbox while stopped. */
bool rai_game_metadata(const GameIdentity &, RaiGameMetadata &, std::string &error) noexcept;
/** Fill process and module fields while owning a verified stopped-process lease. */
bool rai_game_process_snapshot(const GameIdentity &, const RaiGameMetadata &,
                               RaiGameSnapshot &, std::string &error) noexcept;
/** Narrow transform invoked only for the exact SELF or extracted ELF Rai SHA and PS4.
 * Caller owns any target mappings; module publication is a separate action. */
struct RaiStageAdapter {
  static bool accepts_exact_source(std::string_view sha256, std::string_view title,
                                    int platform) noexcept {
    return (sha256 == "15dde8a561a6d50f7387815e16c78df678b06aa7bdb53b8e0d8fa5c44e94b26c" ||
            sha256 == "e7b25eb1c6245f1ef3f97a301c23fa53baa33ce46a09c9834208118dd57b5358") &&
           title.size() == 9 && title.starts_with("CUSA") &&
           (platform == 0 || platform == 4);
  }
  void *context = nullptr;
  bool (*prepare)(void *, const GameIdentity &, std::vector<unsigned char> &, std::string &) = nullptr;
  std::string source_sha256;
  uint64_t nonce = 0;
  bool adapted = false;
};
bool stage_module(const GameIdentity &identity, std::string_view source,
                  uint32_t host_sdk, std::string &path,
                  std::string &fingerprint, std::string &error,
                  RaiStageAdapter *rai = nullptr) noexcept;
}
