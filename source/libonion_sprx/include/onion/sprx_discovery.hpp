#pragma once

#include <onion/sprx_catalog.hpp>

namespace onion::sprx {

inline constexpr size_t kSprxDiscoveryPathBytes = 255;
inline constexpr size_t kSprxDiscoveryCandidateLimit = 128;
inline constexpr size_t kSprxDiscoveryScanLimit = 2048;

struct SprxDiscoveryResult {
  std::vector<SprxInventoryEntry> candidates;
  std::string error;
};

/** Exactly file. + first 12 SHA256 bytes of the absolute UTF-8 path. */
std::string sprx_file_id(std::string_view path);
/** Rejects symlink components, unsupported suffixes, control/invalid UTF-8,
 * and files outside the explicitly supplied nonrecursive roots. */
bool valid_discovered_sprx(std::string_view path,
                           const std::vector<std::string> &roots,
                           std::string *error = nullptr);
SprxDiscoveryResult discover_sprx_files(const SprxCatalog &catalog,
                                        const std::vector<std::string> &roots,
                                        size_t candidate_limit = kSprxDiscoveryCandidateLimit,
                                        size_t scan_limit = kSprxDiscoveryScanLimit);

} // namespace onion::sprx
