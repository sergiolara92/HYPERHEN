/* SPDX-License-Identifier: GPL-3.0-or-later
 * Sandbox/staging workflow adapted from TacHen's game-source.c and
 * module-storage.c. Original notices: third_party/tachen-plugin-runtime.
 */
#include "plugin-filesystem.h"
#include <onion/plugin_staging.hpp>
#include <onion/log.h>
#include <onion/plugin_metadata.hpp>
#include <onion/pt.h>
#include "prx-image.h"
#include "process-info.h"
#include "game-info.h"
#include "game-source.h"
extern "C" {
#include "game-runtime/memory-map.h"
}
extern "C" {
#include "sha256.h"
}
#include <ps5/kernel.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

extern "C" int sceKernelGetAppInfo(int, void *);
namespace onion::sprx {
namespace {
struct Fd {
  int value = -1;
  explicit Fd(int fd = -1) : value(fd) {}
  ~Fd() { if(value >= 0) close(value); }
};
// Log the observed failure once, without changing errno or reading again.
#define HHP_IDENTITY_LOG(...) do { const int identity_saved_errno=errno; LOG_WARN(__VA_ARGS__); errno=identity_saved_errno; } while(false)
bool title_for(pid_t pid, std::string &title) {
  alignas(8) unsigned char app[128]{};
  const int status=sceKernelGetAppInfo(pid, app);
  if(status) {
    HHP_IDENTITY_LOG("[plugins] identity capture failed pid=%d phase=app-info status=0x%x errno=%d",pid,static_cast<unsigned>(status),errno);
    return false;
  }
  HhpAppInfo decoded{};
  const int decoded_status=hhp_app_info_decode(app, 96, &decoded);
  if(decoded_status) {
    HHP_IDENTITY_LOG("[plugins] identity capture failed pid=%d phase=app-decode status=%d",pid,decoded_status);
    return false;
  }
  title = decoded.title;
  return true;
}
bool stage_failure(std::string &error, const char *phase, const char *operation,
                   long long status, int operation_errno, const char *detail = nullptr) {
  const int saved_errno = errno;
  char message[384];
  snprintf(message, sizeof(message), "Plugin staging phase=%s operation=%s status=%lld errno=%d%s%.192s",
           phase, operation, status, operation_errno, detail ? ": " : "", detail ? detail : "");
  error = message;
  errno = saved_errno;
  return false;
}
bool transfer(int fd, void *data, size_t size, bool writing,
              std::string *error = nullptr, const char *phase = "file-transfer") {
  auto *p = static_cast<unsigned char *>(data);
  while(size) {
    ssize_t n = writing ? write(fd, p, size) : read(fd, p, size);
    const int operation_errno = errno;
    if(n < 0 && operation_errno == EINTR) continue;
    if(n <= 0) {
      if(error) stage_failure(*error, phase, writing ? "write" : "read", n,
                             n < 0 ? operation_errno : 0, n == 0 ? "zero-byte transfer" : nullptr);
      return false;
    }
    p += n; size -= static_cast<size_t>(n);
  }
  return true;
}
int child(int fd, const char *name, mode_t mode, std::string &error, const char *phase) {
  const int made = hhp_fs_mkdirat(fd, name, mode), made_errno = errno;
  if(made && made_errno != EEXIST) {
    stage_failure(error, phase, "mkdirat", made, made_errno); return -1;
  }
  int out = hhp_fs_openat(fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW, 0);
  struct stat st{};
  if(out < 0) { stage_failure(error, phase, "openat", out, errno); return -1; }
  const int stated = fstat(out, &st), stated_errno = errno;
  if(stated) {
    stage_failure(error, phase, "fstat", stated, stated_errno); close(out); return -1;
  }
  if(!S_ISDIR(st.st_mode)) {
    stage_failure(error, phase, "directory-type", -1, 0); close(out); return -1;
  }
  if(st.st_mode & 0022) {
    stage_failure(error, phase, "directory-write-policy", -1, EPERM); close(out); return -1;
  }
  if(st.st_uid != 0 && st.st_uid != getuid()) {
    stage_failure(error, phase, "directory-owner-policy", -1, EPERM); close(out); return -1;
  }
  const int changed = fchmod(out, mode), changed_errno = errno;
  if(changed) {
    stage_failure(error, phase, "fchmod", changed, changed_errno); close(out); return -1;
  }
  return out;
}
bool publish_new_file(int directory, const char *temporary, const char *final,
                      void *data, size_t size, std::string &error) {
  Fd output(hhp_fs_openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600));
  if(output.value < 0)
    return stage_failure(error, "module-create", "openat", output.value, errno);
  bool ok = transfer(output.value, data, size, true, &error, "module-write");
  if(ok) {
    const int status = fchmod(output.value, 0555), operation_errno = errno;
    if(status) ok = stage_failure(error, "module-permissions", "fchmod", status, operation_errno);
  }
  if(ok) {
    const int status = fsync(output.value), operation_errno = errno;
    if(status) ok = stage_failure(error, "module-sync", "fsync", status, operation_errno);
  }
  if(ok) {
    const int status = hhp_fs_renameat(directory, temporary, directory, final), operation_errno = errno;
    if(status) ok = stage_failure(error, "module-publish", "renameat", status, operation_errno);
  }
  if(ok) {
    const int status = fsync(directory), operation_errno = errno;
    if(status) ok = stage_failure(error, "directory-sync", "fsync", status, operation_errno);
  }
  if(!ok) { hhp_fs_unlinkat(directory, temporary, 0); return false; }
  return true;
}
int sandbox(const GameIdentity &id, std::string &path, bool &global) {
  if(!same_game(id)) return -1;
  intptr_t original = kernel_get_proc_rootdir(id.pid);
  intptr_t expected = original, root = kernel_get_root_vnode();
  if(!original || !root) return -1;
  if(expected == root) {
    intptr_t jail = kernel_get_proc_jaildir(id.pid);
    if(jail && jail != root) expected = jail;
  }
  const bool lookup_global = expected == root;
  global = original == root;
  DIR *dir = opendir("/mnt/sandbox");
  if(!dir) return -1;
  int selected = -1;
  unsigned count = 0;
  while(dirent *entry = readdir(dir)) {
    if(++count > 4096) { if(selected >= 0) close(selected); selected = -1; break; }
    std::string_view name(entry->d_name);
    if(!name.starts_with(id.title + "_") || name.size() < 11 || name.size() > 16)
      continue;
    if(!std::all_of(name.begin()+10, name.end(), [](char c){return c>='0' && c<='9';}))
      continue;
    int candidate = hhp_fs_openat(dirfd(dir), entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW, 0);
    if(candidate < 0) continue;
    if(!lookup_global && kernel_get_proc_file(getpid(), candidate) != expected) {
      close(candidate); continue;
    }
    if(lookup_global) {
      Fd app(hhp_fs_openat(candidate, "app0", O_RDONLY | O_DIRECTORY | O_NOFOLLOW, 0));
      if(app.value < 0) { close(candidate); continue; }
      if(selected >= 0) { close(candidate); close(selected); selected = -1; break; }
    }
    selected = candidate; path = "/mnt/sandbox/" + std::string(name);
    if(!lookup_global) break;
  }
  closedir(dir);
  if(selected >= 0 && (!same_game(id) || kernel_get_proc_rootdir(id.pid) != original)) {
    close(selected); return -1;
  }
  return selected;
}
}
bool capture_game(pid_t pid, GameIdentity &id) noexcept {
  kinfo_proc process{};
  std::string title;
  if(pid <= 1) {
    HHP_IDENTITY_LOG("[plugins] identity capture failed pid=%d phase=pid",pid);
    return false;
  }
  HhpProcessInfoDiagnostic diagnostic{};
  if(hhp_process_info_ex(pid, &process, &diagnostic)) {
    HHP_IDENTITY_LOG("[plugins] identity capture failed pid=%d phase=process-info step=%s status=%d errno=%d requested=%zu returned=%zu declared=%d observed-pid=%d",pid,diagnostic.stage?diagnostic.stage:"unknown",diagnostic.status,diagnostic.error,diagnostic.requested,diagnostic.returned,diagnostic.declared,diagnostic.observed_pid);
    return false;
  }
  if(process.ki_start.tv_sec <= 0 || process.ki_start.tv_usec < 0 || process.ki_start.tv_usec >= 1000000) {
    HHP_IDENTITY_LOG("[plugins] identity capture failed pid=%d phase=start-time seconds=%lld microseconds=%lld",pid,static_cast<long long>(process.ki_start.tv_sec),static_cast<long long>(process.ki_start.tv_usec));
    return false;
  }
  if(!title_for(pid, title)) return false;
  id = {pid, process.ki_start.tv_sec, process.ki_start.tv_usec, std::move(title)};
  id.rootdir = kernel_get_proc_rootdir(pid); id.jaildir = kernel_get_proc_jaildir(pid);
  if(!id.rootdir) {
    HHP_IDENTITY_LOG("[plugins] identity capture failed pid=%d phase=rootdir-unavailable root=%llx jail=%llx",pid,static_cast<unsigned long long>(id.rootdir),static_cast<unsigned long long>(id.jaildir));
    return false;
  }
  return true;
}
bool same_game(const GameIdentity &id) noexcept {
  GameIdentity now;
  if(!capture_game(id.pid, now)) return false;
  if(id.seconds != now.seconds || id.microseconds != now.microseconds) {
    HHP_IDENTITY_LOG("[plugins] identity mismatch pid=%d field=start-time expected=%lld:%lld observed=%lld:%lld",id.pid,static_cast<long long>(id.seconds),static_cast<long long>(id.microseconds),static_cast<long long>(now.seconds),static_cast<long long>(now.microseconds));
    return false;
  }
  if(id.title != now.title) {
    HHP_IDENTITY_LOG("[plugins] identity mismatch pid=%d field=title expected=%s observed=%s",id.pid,id.title.c_str(),now.title.c_str());
    return false;
  }
  if(id.rootdir != now.rootdir) {
    HHP_IDENTITY_LOG("[plugins] identity mismatch pid=%d field=rootdir expected=%llx observed=%llx",id.pid,static_cast<unsigned long long>(id.rootdir),static_cast<unsigned long long>(now.rootdir));
    return false;
  }
  if(id.jaildir != now.jaildir) {
    HHP_IDENTITY_LOG("[plugins] identity mismatch pid=%d field=jaildir expected=%llx observed=%llx",id.pid,static_cast<unsigned long long>(id.jaildir),static_cast<unsigned long long>(now.jaildir));
    return false;
  }
  return true;
}
#undef HHP_IDENTITY_LOG
bool game_version(const GameIdentity &id, std::string &version, std::string &hash) noexcept {
  timeval boot{}; size_t boot_size=sizeof(boot);int mib[]={CTL_KERN,KERN_BOOTTIME};
  if(sysctl(mib,2,&boot,&boot_size,nullptr,0) || boot_size!=sizeof(boot) || !same_game(id)) return false;
  char generation[128];snprintf(generation,sizeof(generation),"%lld:%lld/%d/%lld:%lld",
    static_cast<long long>(boot.tv_sec),static_cast<long long>(boot.tv_usec),id.pid,
    static_cast<long long>(id.seconds),static_cast<long long>(id.microseconds));
  unsigned char *bytes=nullptr;size_t size=0;char reason[192]{};
  if(hhp_game_metadata_read(id.pid,generation,id.title.c_str(),&bytes,&size,reason,sizeof(reason))) {
    LOG_WARN("[plugins] game metadata unavailable pid=%d: %s",id.pid,reason);return false;
  }
  const bool ok=parse_game_metadata({bytes,size},id.title,version);
  if(ok) {
    SHA256_CTX sha;unsigned char digest[32];char hex[65]{};
    sha256_init(&sha);sha256_update(&sha,bytes,size);sha256_final(&sha,digest);
    for(unsigned i=0;i<32;++i)snprintf(hex+i*2,3,"%02x",digest[i]);hash=hex;
  }
  free(bytes);return ok && same_game(id);
}
bool rai_game_metadata(const GameIdentity &id, RaiGameMetadata &out, std::string &error) noexcept {
  out = {};
  error = "Rai game metadata unavailable before attach";
  if (!id.title.starts_with("CUSA") || !same_game(id)) return false;
  std::string version, metadata_hash;
  if (!game_version(id, version, metadata_hash)) return false;
  timeval boot{}; size_t boot_size = sizeof(boot); int boot_mib[] = {CTL_KERN, KERN_BOOTTIME};
  if (sysctl(boot_mib, 2, &boot, &boot_size, nullptr, 0) || boot_size != sizeof(boot)) return false;
  char generation[128];
  snprintf(generation, sizeof(generation), "%lld:%lld/%d/%lld:%lld",
    static_cast<long long>(boot.tv_sec), static_cast<long long>(boot.tv_usec), id.pid,
    static_cast<long long>(id.seconds), static_cast<long long>(id.microseconds));
  unsigned char *raw = nullptr; size_t size = 0; char reason[192]{}; int registered = 0;
  if (hhp_game_metadata_read_ex(id.pid, generation, id.title.c_str(), &raw, &size,
                               reason, sizeof(reason), &registered)) { error = reason; return false; }
  if (!raw || size < 20 || size > 1024*1024) { free(raw); return false; }
  std::vector<unsigned char> bytes(raw, raw + size); free(raw);
  // The metadata reader permits a registered SFO only for a missing or
  // virtual mounted SFO, and verifies it predates this exact game process.
  // Keep the title, version, CONTENT_ID and repeated-hash checks below.
  if (!parse_game_metadata(bytes, id.title, version)) {
    error = "Rai requires verified PS4 SFO metadata"; return false;
  }
  if (registered)
    LOG_INFO("[plugins] Rai snapshot uses pre-launch registered SFO pid=%d tid=%s", id.pid, id.title.c_str());
  SHA256_CTX sha; unsigned char digest[32]; char actual_hash[65]{};
  sha256_init(&sha); sha256_update(&sha, bytes.data(), bytes.size()); sha256_final(&sha, digest);
  for (unsigned i = 0; i < 32; ++i) snprintf(actual_hash + i*2, 3, "%02x", digest[i]);
  if (metadata_hash != actual_hash) { error = "game metadata changed during Rai snapshot"; return false; }
  const auto u16 = [&](size_t at) { uint16_t v = 0; if (at <= bytes.size() && sizeof(v) <= bytes.size()-at) memcpy(&v, bytes.data()+at, sizeof(v)); return v; };
  const auto u32 = [&](size_t at) { uint32_t v = 0; if (at <= bytes.size() && sizeof(v) <= bytes.size()-at) memcpy(&v, bytes.data()+at, sizeof(v)); return v; };
  // The strict version parser already validated every table bound, duplicate
  // key and overlapping value span. Extract the additional typed string.
  const size_t keys = u32(8), values = u32(12), count = u32(16);
  std::string content; unsigned contents = 0;
  for (size_t i = 0; i < count; ++i) {
    const size_t at = 20 + i*16, key = keys + u16(at);
    const char *name = reinterpret_cast<const char *>(bytes.data()+key);
    if (strcmp(name, "CONTENT_ID")) continue;
    const size_t length = u32(at+4), offset = u32(at+12);
    if (++contents != 1 || (u16(at+2) != 4 && u16(at+2) != 0x204) || !length || length > 64) return false;
    const char *value = reinterpret_cast<const char *>(bytes.data()+values+offset);
    const size_t used = strnlen(value, length);
    if (!used || used >= length) return false;
    for (size_t j = used; j < length; ++j) if (value[j]) return false;
    content.assign(value, used);
  }
  if (contents != 1 || content.size() < 16 || content.substr(7, 9) != id.title ||
      !std::all_of(content.begin(), content.end(), [](unsigned char c){ return c >= 0x21 && c <= 0x7e; })) {
    error = "verified game CONTENT_ID is missing or inconsistent"; return false;
  }
  if (!same_game(id)) { error = "Game instance changed during Rai metadata read"; return false; }
  out.title = id.title; out.content = std::move(content); out.version = std::move(version);
  out.metadata_hash = std::move(metadata_hash);
  error.clear(); return true;
}
bool rai_game_process_snapshot(const GameIdentity &id, const RaiGameMetadata &metadata,
                               RaiGameSnapshot &out, std::string &error) noexcept {
  out = {};
  error = "Rai process snapshot unavailable after attach";
  if (metadata.title != id.title || metadata.content.empty() || metadata.version.size() != 5 ||
      metadata.metadata_hash.size() != 64) {
    error = "Rai metadata snapshot is invalid after attach"; return false;
  }
  if (!same_game(id)) { error = "Rai game identity changed after attach"; return false; }
  kinfo_proc process{};
  if (hhp_process_info(id.pid, &process)) { error = "Rai process information is unavailable"; return false; }
  if (process.ki_start.tv_sec != id.seconds || process.ki_start.tv_usec != id.microseconds) {
    error = "Rai process generation changed after attach"; return false;
  }
  const size_t name_size = strnlen(process.ki_comm, sizeof(process.ki_comm));
  if (!name_size || name_size >= sizeof(process.ki_comm) || name_size >= 40) {
    error = "Rai process name exceeds snapshot bounds"; return false;
  }
  const auto read_kernel = [](void *, uint64_t address, void *data, size_t length) {
    return kernel_copyout(address, data, length);
  };
  TacMemoryReader reader{nullptr, read_kernel}; TacMemoryModule module{};
  const intptr_t proc = kernel_get_proc(id.pid);
  if (!proc) { error = "Rai kernel process is unavailable"; return false; }
  // Read the path from the same bounded dynlib record that identifies eboot.
  // kern.proc.pathname is not approved for this daemon on some PS4 routes.
  char pathname[64]{};
  LOG_INFO("[plugins] Rai snapshot phase=module-path begin pid=%d", id.pid);
  const int module_status = tac_memory_module_path(&reader, proc, "eboot.bin", &module,
                                                  pathname, sizeof(pathname));
  LOG_INFO("[plugins] Rai snapshot phase=module-path end pid=%d status=%d", id.pid, module_status);
  if (module_status) {
    char reason[128]; snprintf(reason, sizeof(reason), "Rai eboot path lookup failed (status %d)", module_status);
    error = reason; return false;
  }
  const std::string_view module_path(pathname);
  if (!module_path.ends_with("/eboot.bin") ||
      !std::all_of(module_path.begin(), module_path.end(), [](unsigned char c){ return c >= 0x21 && c <= 0x7e; })) {
    error = "Rai eboot path is invalid"; return false;
  }
  LOG_INFO("[plugins] Rai snapshot phase=base-entry begin pid=%d", id.pid);
  const intptr_t base = kernel_dynlib_mapbase_addr(id.pid, module.handle);
  const intptr_t entry = kernel_dynlib_entry_addr(id.pid, module.handle);
  LOG_INFO("[plugins] Rai snapshot phase=base-entry end pid=%d", id.pid);
  if (base <= 0 || static_cast<uint64_t>(base) != module.base) {
    error = "Rai eboot base differs from its dynlib record"; return false;
  }
  unsigned char probe[4]{};
  if (entry < base || module.size < sizeof(probe) ||
      static_cast<uint64_t>(entry-base) > module.size-sizeof(probe)) {
    error = "Rai eboot entry is outside its mapped module"; return false;
  }
  LOG_INFO("[plugins] Rai snapshot phase=mapping begin pid=%d", id.pid);
  const int base_protection = kernel_get_vmem_protection(id.pid, base, 1);
  const int entry_protection = kernel_get_vmem_protection(id.pid, entry, sizeof(probe));
  LOG_INFO("[plugins] Rai snapshot phase=mapping end pid=%d base-prot=%d entry-prot=%d",
           id.pid, base_protection, entry_protection);
  if (base_protection < 0 || !(base_protection & PROT_READ) ||
      entry_protection < 0 || !(entry_protection & PROT_EXEC)) {
    error = "Rai eboot mappings are not readable/executable"; return false;
  }
  // A PS4 PT_LOAD can map code from file offset 0x4000 at virtual address zero.
  // The live mapbase therefore need not contain an ELF header. Validate the
  // executable mapping and readable entry bytes without imposing header data.
  LOG_INFO("[plugins] Rai snapshot phase=entry-read begin pid=%d", id.pid);
  if (pt_copyout(id.pid, entry, probe, sizeof(probe))) {
    error = "Rai eboot entry could not be read"; return false;
  }
  LOG_INFO("[plugins] Rai snapshot phase=entry-read end pid=%d", id.pid);
  TacMemoryModule after{}; char after_path[64]{};
  if (tac_memory_module_path(&reader, proc, "eboot.bin", &after, after_path, sizeof(after_path)) ||
      after.object != module.object || after.base != module.base || after.size != module.size ||
      after.handle != module.handle || strcmp(after_path, pathname) || !same_game(id)) {
    error = "Rai eboot identity changed during snapshot"; return false;
  }
  out.pid = id.pid; out.name.assign(process.ki_comm, name_size); out.path = pathname;
  out.base = static_cast<uint64_t>(base);
  LOG_INFO("[plugins] Rai snapshot phase=verified pid=%d", id.pid);
  error.clear(); return true;
}
bool stage_module(const GameIdentity &id, std::string_view source,
                  uint32_t sdk, std::string &path, std::string &hash,
                  std::string &error, RaiStageAdapter *rai) noexcept {
  error = "plugin staging failed";
  const std::string filename(source);
  Fd input(open(filename.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK));
  struct stat before{}, after{};
  if(input.value < 0) return stage_failure(error, "source-open", "open", input.value, errno);
  const int source_stat = fstat(input.value, &before), source_stat_errno = errno;
  if(source_stat) return stage_failure(error, "source-stat", "fstat", source_stat, source_stat_errno);
  if(!S_ISREG(before.st_mode)) return stage_failure(error, "source-type", "regular-file", -1, 0);
  if(before.st_size < 64 || before.st_size > 128 * 1024 * 1024)
    return stage_failure(error, "source-size", "bounds", -1, 0);
  std::vector<unsigned char> bytes(static_cast<size_t>(before.st_size));
  if(!transfer(input.value, bytes.data(), bytes.size(), false, &error, "source-read")) return false;
  const int source_restat = fstat(input.value, &after), source_restat_errno = errno;
  if(source_restat) return stage_failure(error, "source-recheck", "fstat", source_restat, source_restat_errno);
  if(after.st_size != before.st_size ||
     after.st_mtim.tv_sec != before.st_mtim.tv_sec || after.st_mtim.tv_nsec != before.st_mtim.tv_nsec ||
     after.st_ctim.tv_sec != before.st_ctim.tv_sec || after.st_ctim.tv_nsec != before.st_ctim.tv_nsec)
    return stage_failure(error, "source-recheck", "unchanged-file", -1, 0);
  SHA256_CTX original_sha; unsigned char original_digest[32]; char original_text[65]{};
  sha256_init(&original_sha); sha256_update(&original_sha, bytes.data(), bytes.size()); sha256_final(&original_sha, original_digest);
  for (unsigned i = 0; i < 32; ++i) snprintf(original_text+i*2, 3, "%02x", original_digest[i]);
  int platform = 0;
  char reason[192]{};
  const int inspected = hhp_prx_inspect(bytes.data(), bytes.size(), &platform, reason, sizeof(reason));
  if(inspected) return stage_failure(error, "source-container", "inspect", inspected, 0, reason);
  const bool ps5 = id.title.starts_with("PPS") || id.title.starts_with("MOUU");
  if(platform && platform != (ps5 ? 5 : 4))
    return stage_failure(error, "source-platform", "matching-console", -1, 0);
  constexpr const char *rai_self_sha = "15dde8a561a6d50f7387815e16c78df678b06aa7bdb53b8e0d8fa5c44e94b26c";
  constexpr const char *rai_raw_sha = "e7b25eb1c6245f1ef3f97a301c23fa53baa33ce46a09c9834208118dd57b5358";
  const bool exact_rai = !strcmp(original_text, rai_self_sha) || !strcmp(original_text, rai_raw_sha);
  if (rai) { rai->adapted = false; rai->source_sha256 = original_text; rai->nonce = 0; }
  if (exact_rai) {
    if (ps5 || !RaiStageAdapter::accepts_exact_source(original_text, id.title, platform) ||
        !rai || !rai->prepare || !same_game(id)) {
      return stage_failure(error, "rai-context", "verified-ps4-game", -1, 0);
    }
    if (!rai->prepare(rai->context, id, bytes, error)) {
      if(error.empty()) stage_failure(error, "rai-prepare", "callback", -1, 0);
      return false;
    }
    if (!rai->nonce) return stage_failure(error, "rai-prepare", "nonzero-nonce", -1, 0);
    if (!same_game(id)) return stage_failure(error, "rai-prepare", "post-callback-identity", -1, 0);
    rai->adapted = true;
  }
  uint32_t original_sdk = 0;
  int adapted = ps5 ? hhp_prx_match_host_sdk(bytes.data(), bytes.size(), sdk,
                                            kernel_get_fw_version(), &original_sdk) : 0;
  if(adapted < 0) return stage_failure(error, "sdk-adaptation", "module-metadata", adapted, 0);
  unsigned char digest[32];
  SHA256_CTX sha; sha256_init(&sha); sha256_update(&sha, bytes.data(), bytes.size());
  if (exact_rai) {
    // Keep the original callback's strict64hex filename grammar while making
    // every address-bearing staged identity request/generation-specific.
    constexpr char tag[] = "HyperHEN-R54-Rai-stage-v1";
    sha256_update(&sha, reinterpret_cast<const unsigned char *>(tag), sizeof(tag));
    sha256_update(&sha, reinterpret_cast<const unsigned char *>(&rai->nonce), sizeof(rai->nonce));
    sha256_update(&sha, reinterpret_cast<const unsigned char *>(&id.pid), sizeof(id.pid));
    sha256_update(&sha, reinterpret_cast<const unsigned char *>(&id.seconds), sizeof(id.seconds));
    sha256_update(&sha, reinterpret_cast<const unsigned char *>(&id.microseconds), sizeof(id.microseconds));
  }
  sha256_final(&sha, digest);
  char text[65]{};
  for(unsigned i=0; i<32; ++i) snprintf(text+i*2, 3, "%02x", digest[i]);
  hash = text;
  char variant[40]; snprintf(variant, sizeof(variant), "v1-p%d-%08x", ps5?5:4, sdk);
  Fd home(open("/data/HyperHEN", O_RDONLY | O_DIRECTORY | O_NOFOLLOW));
  if(home.value < 0) return stage_failure(error, "backing-home", "open", home.value, errno);
  Fd cache(child(home.value, "plugin-modules", 0700, error, "backing-cache"));
  if(cache.value < 0) return false;
  Fd backing(child(cache.value, variant, 0755, error, "backing-variant"));
  if(backing.value < 0) return false;
  std::string root_path; bool global = false;
  Fd game(sandbox(id, root_path, global));
  if(game.value < 0) return stage_failure(error, "game-sandbox", "verified-lookup", -1, 0);
  const int target_mkdir = hhp_fs_mkdirat(game.value, "hyperhen-modules", 0755), target_mkdir_errno = errno;
  if(target_mkdir && target_mkdir_errno != EEXIST)
    return stage_failure(error, "mount-directory", "mkdirat", target_mkdir, target_mkdir_errno);
  Fd target(hhp_fs_openat(game.value, "hyperhen-modules", O_RDONLY | O_DIRECTORY | O_NOFOLLOW, 0));
  struct statfs fs{};
  if(target.value < 0) return stage_failure(error, "mount-directory", "openat", target.value, errno);
  const int target_stat = hhp_fs_fstatfs(target.value, &fs), target_stat_errno = errno;
  if(target_stat) return stage_failure(error, "mount-inspect", "fstatfs", target_stat, target_stat_errno);
  const std::string from = "/data/HyperHEN/plugin-modules/" + std::string(variant);
  const std::string to = root_path + "/hyperhen-modules";
  if(to != fs.f_mntonname) {
    if(!same_game(id)) return stage_failure(error, "mount-prepare", "game-identity", -1, 0);
    const char *v[] = {"fstype", "nullfs", "from", from.c_str(), "fspath", to.c_str()};
    iovec io[6];
    for(unsigned i=0;i<6;++i) { io[i].iov_base = const_cast<char *>(v[i]); io[i].iov_len = strlen(v[i])+1; }
    const auto mounted = nmount(io, 6, MNT_RDONLY);
    const int mount_errno = errno;
    if(mounted) return stage_failure(error, "mount-create", "nmount", mounted, mount_errno);
    close(target.value); target.value = hhp_fs_openat(game.value, "hyperhen-modules", O_RDONLY | O_DIRECTORY | O_NOFOLLOW, 0);
    if(target.value < 0) return stage_failure(error, "mount-reopen", "openat", target.value, errno);
    const int mounted_stat = hhp_fs_fstatfs(target.value, &fs), mounted_stat_errno = errno;
    if(mounted_stat) return stage_failure(error, "mount-reinspect", "fstatfs", mounted_stat, mounted_stat_errno);
  }
  if(strcmp(fs.f_fstypename, "nullfs") || from != fs.f_mntfromname || to != fs.f_mntonname ||
     !(fs.f_flags & MNT_RDONLY) || (fs.f_flags & MNT_NOEXEC) || !same_game(id)) {
    return stage_failure(error, "mount-verify", "identity-and-permissions", -1, 0);
  }
  const std::string suffix = filename.ends_with(".sprx") ? ".sprx" : ".prx";
  const std::string final = hash + suffix;
  Fd existing(hhp_fs_openat(backing.value, final.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK, 0));
  const int existing_errno = errno;
  if(existing.value >= 0) {
    struct stat old{};
    const int existing_stat = fstat(existing.value, &old), existing_stat_errno = errno;
    if(existing_stat) return stage_failure(error, "cached-module", "fstat", existing_stat, existing_stat_errno);
    if(!S_ISREG(old.st_mode) || old.st_size != before.st_size || (old.st_mode & 0222))
      return stage_failure(error, "cached-module", "type-size-permissions", -1, 0);
    std::vector<unsigned char> saved(bytes.size());
    if(!transfer(existing.value, saved.data(), saved.size(), false, &error, "cached-module-read")) return false;
    if(saved != bytes) return stage_failure(error, "cached-module", "matching-content", -1, 0);
    const int existing_chmod = fchmod(existing.value, 0555), existing_chmod_errno = errno;
    if(existing_chmod) return stage_failure(error, "cached-module", "fchmod", existing_chmod, existing_chmod_errno);
  } else {
    if(existing_errno != ENOENT) return stage_failure(error, "cached-module", "openat", existing.value, existing_errno);
    char temp[80]; snprintf(temp, sizeof(temp), ".%s-%08x.part", hash.c_str(), arc4random());
    if(!publish_new_file(backing.value, temp, final.c_str(), bytes.data(), bytes.size(), error)) return false;
  }
  /* A game whose root was deliberately restored to the global root resolves
   * the verified sandbox's full path, not a nonexistent global /hyperhen-modules. */
  path = (global ? to : "/hyperhen-modules") + "/" + final;
  if (exact_rai) {
    SHA256_CTX content_sha; unsigned char content_digest[32]; char content_text[65]{};
    sha256_init(&content_sha); sha256_update(&content_sha, bytes.data(), bytes.size()); sha256_final(&content_sha, content_digest);
    for (unsigned i=0;i<32;++i) snprintf(content_text+i*2,3,"%02x",content_digest[i]);
    LOG_INFO("[plugins] Rai staged pid=%d source-sha256=%s patched-sha256=%s staged-identity=%s nonce=%llx",
      id.pid,original_text,content_text,hash.c_str(),static_cast<unsigned long long>(rai->nonce));
  }
  LOG_INFO("[plugins] staged pid=%d title=%s sdk=%08x module-sdk=%08x adapted=%d sha256=%s",
           id.pid, id.title.c_str(), sdk, original_sdk, adapted, hash.c_str());
  error.clear();
  if(!same_game(id)) return stage_failure(error, "staging-complete", "game-identity", -1, 0);
  return true;
}
}
