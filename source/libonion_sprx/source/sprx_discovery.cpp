#include "plugin-filesystem.h"
#include <onion/sprx_discovery.hpp>

extern "C" {
#include "sha256.h"
}
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace onion::sprx {
namespace {

bool text_path(std::string_view text) {
  if (text.empty() || text.size() > kSprxDiscoveryPathBytes || text.front() != '/') return false;
  for (size_t i = 0; i < text.size();) {
    const unsigned char first = text[i++];
    if (first < 0x20 || first == 0x7f) return false;
    if (first < 0x80) continue;
    unsigned count = 0; uint32_t value = 0; uint32_t minimum = 0;
    if (first >= 0xc2 && first <= 0xdf) { count = 1; value = first & 0x1f; minimum = 0x80; }
    else if (first >= 0xe0 && first <= 0xef) { count = 2; value = first & 0x0f; minimum = 0x800; }
    else if (first >= 0xf0 && first <= 0xf4) { count = 3; value = first & 7; minimum = 0x10000; }
    else return false;
    if (count > text.size() - i) return false;
    while (count--) {
      const unsigned char next = text[i++];
      if ((next & 0xc0) != 0x80) return false;
      value = (value << 6) | (next & 0x3f);
    }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff) ||
        (value >= 0x80 && value <= 0x9f)) return false;
  }
  size_t begin = 1;
  while (begin <= text.size()) {
    const auto end = text.find('/', begin);
    const auto component = text.substr(begin, end == text.npos ? text.size() - begin : end - begin);
    if (component.empty() || component == "." || component == "..") return false;
    if (end == text.npos) break;
    begin = end + 1;
  }
  return true;
}

bool extension(std::string_view name) {
  const auto dot = name.find_last_of('.');
  if (dot == name.npos) return false;
  std::string suffix(name.substr(dot));
  for (auto &c : suffix) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return suffix == ".prx" || suffix == ".sprx";
}

int directory(std::string_view root) {
  if (!text_path(root)) { errno = EINVAL; return -1; }
  // Walk with openat and O_NOFOLLOW so neither parent directories nor the
  // final directory can redirect discovery through a symbolic link.
  int fd = open("/", O_RDONLY | O_DIRECTORY);
  if (fd < 0) return -1;
  size_t start = 1;
  while (start < root.size()) {
    const auto slash = root.find('/', start);
    const std::string component(root.substr(start, slash == root.npos ? root.size() - start : slash - start));
    const int next = hhp_fs_openat(fd, component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW, 0);
    const int error = errno;
    (void)close(fd);
    if (next < 0) { errno = error; return -1; }
    fd = next;
    if (slash == root.npos) break;
    start = slash + 1;
  }
  return fd;
}

bool regular_file(int root, const char *leaf, struct stat &info) {
  const int fd = hhp_fs_openat(root, leaf, O_RDONLY | O_NOFOLLOW | O_NONBLOCK, 0);
  if (fd < 0) return false;
  const bool valid = fstat(fd, &info) == 0 && S_ISREG(info.st_mode);
  const int error = errno;
  (void)close(fd);
  if (!valid) errno = error ? error : EINVAL;
  return valid;
}

} // namespace

std::string sprx_file_id(std::string_view path) {
  SHA256_CTX context; unsigned char digest[32];
  sha256_init(&context);
  sha256_update(&context, reinterpret_cast<const unsigned char *>(path.data()), path.size());
  sha256_final(&context, digest);
  constexpr char hex[] = "0123456789abcdef";
  std::string id = "file.";
  for (size_t i = 0; i < 12; ++i) { id += hex[digest[i] >> 4]; id += hex[digest[i] & 15]; }
  return id;
}

bool valid_discovered_sprx(std::string_view path, const std::vector<std::string> &roots,
                           std::string *error) {
  if (error) error->clear();
  const auto slash = path.find_last_of('/');
  if (!text_path(path) || slash == path.npos || !extension(path.substr(slash + 1)) ||
      std::find(roots.begin(), roots.end(), path.substr(0, slash)) == roots.end()) {
    if (error) *error = "plugin path is outside discovery roots or has an invalid name";
    return false;
  }
  const int root = directory(path.substr(0, slash));
  if (root < 0) { if (error) *error = "cannot open plugin directory"; return false; }
  struct stat info{};
  const std::string leaf(path.substr(slash + 1));
  const bool ok = regular_file(root, leaf.c_str(), info);
  (void)close(root);
  if (!ok && error) *error = "plugin is not a readable regular file";
  return ok;
}

SprxDiscoveryResult discover_sprx_files(const SprxCatalog &catalog,
                                        const std::vector<std::string> &roots,
                                        size_t candidate_limit, size_t scan_limit) {
  SprxDiscoveryResult result;
  std::vector<SprxInventoryEntry> found;
  size_t scanned = 0;
  for (const auto &root_path : roots) {
    // An optional plugin directory may not have been created yet. Check that
    // case before openat, whose O_DIRECTORY error differs across runtimes.
    struct stat root_info{};
    if (lstat(root_path.c_str(), &root_info) != 0 && errno == ENOENT)
      continue;
    const int fd = directory(root_path);
    if (fd < 0) {
      if (errno == ENOENT) continue;
      result.error = "cannot read plugin discovery directory: " + std::string(std::strerror(errno)); return result;
    }
    DIR *dir = fdopendir(fd);
    if (!dir) { const int error = errno; (void)close(fd); result.error = std::strerror(error); return result; }
    errno = 0;
    while (const auto *item = readdir(dir)) {
      const std::string leaf(item->d_name);
      if (leaf == "." || leaf == "..") { errno = 0; continue; }
      if (++scanned > scan_limit) { (void)closedir(dir); result.error = "plugin discovery scan limit exceeded"; return result; }
      const std::string path = root_path + "/" + leaf;
      if (!extension(leaf) || !text_path(path)) { errno = 0; continue; }
      struct stat info{};
      // lstat-equivalent check ignores directories and links without following.
      if (fstatat(fd, leaf.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
        result.error = "cannot inspect plugin file"; break;
      }
      if (!S_ISREG(info.st_mode)) { errno = 0; continue; }
      if (!regular_file(fd, leaf.c_str(), info)) { result.error = "cannot read plugin file"; break; }
      SprxInventoryEntry entry;
      entry.id = sprx_file_id(path); entry.path = path;
      entry.enabled = false; entry.auto_start = false; entry.registered = false;
      found.push_back(std::move(entry));
      errno = 0;
    }
    const int read_error = errno;
    (void)closedir(dir);
    if (!result.error.empty()) return result;
    if (read_error) { result.error = "cannot enumerate plugin discovery directory"; return result; }
  }
  std::sort(found.begin(), found.end(), [](const auto &a, const auto &b) { return a.path < b.path; });
  std::set<std::pair<dev_t, ino_t>> inodes;
  std::set<std::string> paths;
  std::map<std::string, std::string> ids;
  for (const auto &entry : catalog.entries()) {
    paths.insert(entry.path); ids.emplace(entry.id, entry.path);
    struct stat info{};
    if (lstat(entry.path.c_str(), &info) == 0 && S_ISREG(info.st_mode)) inodes.emplace(info.st_dev, info.st_ino);
  }
  for (const auto &entry : found) {
    if (paths.count(entry.path)) continue;
    const auto same_id = ids.find(entry.id);
    if (same_id != ids.end() && same_id->second != entry.path) {
      result.candidates.clear(); result.error = "plugin discovery ID collision"; return result;
    }
    struct stat info{};
    if (lstat(entry.path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
      result.candidates.clear(); result.error = "plugin discovery file changed; refresh the list"; return result;
    }
    if (!inodes.emplace(info.st_dev, info.st_ino).second) continue;
    paths.insert(entry.path); ids.emplace(entry.id, entry.path);
    if (result.candidates.size() >= candidate_limit) {
      result.candidates.clear(); result.error = "plugin discovery candidate limit exceeded"; return result;
    }
    result.candidates.push_back(entry);
  }
  return result;
}

} // namespace onion::sprx
