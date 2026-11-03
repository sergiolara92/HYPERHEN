#include "sprx_plugin_manager_runtime.hpp"

#include "daemon_ops.hpp"

#include <onion/log.h>
#include <onion/proc_query.h>
#include <onion/sprx_plugin_manager.hpp>
#include <onion/plugin_staging.hpp>
#include <onion/sprx_discovery.hpp>

#include <ps5/kernel.h>
#include <cerrno>
#include <sys/stat.h>
#include <onion/plugin_diagnostic.hpp>

#include <mutex>
#include <string>
#include <vector>

namespace onion::daemon::sprx_plugins {
namespace {

class CurrentGameTarget final : public onion::sprx::ISprxTargetProvider {
public:
  bool current(onion::sprx::SprxTarget *out) noexcept override {
    if (!out)
      return false;
    std::string title_id;
    int app_id = -1;
    const int pid = get_game_pid();
    if (pid <= 1 || !Get_Running_App_TID(title_id, app_id))
      return false;
    out->pid = static_cast<pid_t>(pid);
    out->title_id = std::move(title_id);
    onion::sprx::GameIdentity identity;std::string hash;
    return onion::sprx::capture_game(out->pid,identity) && identity.title==out->title_id &&
           onion::sprx::game_version(identity,out->version,hash);
  }
};

class PreparedTargetAccess final : public onion::sprx::ITargetAccessPolicy {
public:
  onion::sprx::AccessResult prepare(pid_t pid,
                                     std::string_view title) noexcept override {
    if (pid <= 1 || !onion_proc_is_alive(pid) || kernel_get_proc(pid) == 0)
      return {onion::sprx::AccessStatus::PrepareFailed};
    onion::sprx::GameIdentity identity;
    return onion::sprx::capture_game(pid, identity) && identity.title == title
               ? onion::sprx::AccessResult{onion::sprx::AccessStatus::Allowed}
               : onion::sprx::AccessResult{onion::sprx::AccessStatus::Denied};
  }

  onion::sprx::AccessResult restore(pid_t pid,
                                     std::string_view) noexcept override {
    return pid > 1 && onion_proc_is_alive(pid)
               ? onion::sprx::AccessResult{onion::sprx::AccessStatus::Allowed}
               : onion::sprx::AccessResult{onion::sprx::AccessStatus::RestoreFailed};
  }
};

struct Runtime {
  std::mutex mutex;
  onion::sprx::PtraceSprxRuntime transport;
  PreparedTargetAccess access;
  CurrentGameTarget target;
  onion::sprx::SprxPluginManager manager;

  Runtime() : manager(transport, access, target) {}
};

Runtime &runtime() {
  static Runtime value;
  return value;
}

const std::vector<std::string> &discovery_roots() {
  static const std::vector<std::string> roots = {
      "/data/HyperHEN/plugins", "/data/HyperHEN/sprx"};
  return roots;
}

bool catalog_absent() {
  struct stat info{};
  return lstat(kCatalogPath, &info) != 0 && errno == ENOENT;
}

onion::sprx::SprxOperationResult prepare_catalog_directory() {
  // The data root must already exist and cannot be redirected by a link.
  struct stat info{};
  for (const char *path : {"/data", "/data/HyperHEN"})
    if (lstat(path, &info) != 0 || !S_ISDIR(info.st_mode))
      return {false, "plugin data directory is unavailable"};
  if (mkdir("/data/HyperHEN/sprx", 0755) != 0 && errno != EEXIST)
    return {false, "cannot create plugin catalog directory"};
  if (lstat("/data/HyperHEN/sprx", &info) != 0 || !S_ISDIR(info.st_mode))
    return {false, "plugin catalog directory is not a regular directory"};
  return {true, {}};
}

void log_report(const char *operation,
                const onion::sprx::SprxStartupReport &report) {
  for (const auto &issue : report.issues)
    LOG_WARN("[sprx-manager] line=%zu plugin=%s: %s", issue.line,
             issue.plugin_id.empty() ? "-" : issue.plugin_id.c_str(),
             issue.message.c_str());
  size_t loaded = 0;
  size_t failed = 0;
  size_t skipped = 0;
  for (const auto &item : report.results) {
    if (item.skipped_dependency)
      ++skipped;
    else if (item.load.succeeded())
      ++loaded;
    else
      ++failed;
  }
  LOG_INFO("[sprx-manager] %s catalog=%d entries=%zu loaded=%zu failed=%zu skipped=%zu",
           operation, report.catalog_loaded ? 1 : 0, report.results.size(),
           loaded, failed, skipped);
}

void run(const char *operation, bool reload) {
  Runtime &current = runtime();
  std::lock_guard<std::mutex> lock(current.mutex);
  if (reload) {
    std::vector<onion::sprx::SprxCatalogIssue> issues;
    if (!current.manager.load_catalog(kCatalogPath, &issues)) {
      for (const auto &issue : issues)
        LOG_WARN("[sprx-manager] line=%zu: %s", issue.line, issue.message.c_str());
      return;
    }
  }
  const auto report = reload ? current.manager.start() : current.manager.reconcile();
  log_report(operation, report);
}

bool is_current_target(Runtime &current, pid_t pid, uint32_t app_id,
                       std::string_view title_id) {
  onion::sprx::SprxTarget target;
  if (!current.target.current(&target)) {
    LOG_DEBUG("[sprx-manager] lifecycle target unavailable pid=%d",
              static_cast<int>(pid));
    return false;
  }
  if (target.pid != pid || target.title_id != title_id) {
    LOG_DEBUG("[sprx-manager] stale Big App start pid=%d tid=%.*s current_pid=%d current_tid=%s",
              static_cast<int>(pid), static_cast<int>(title_id.size()),
              title_id.data(), static_cast<int>(target.pid),
              target.title_id.c_str());
    return false;
  }
  (void)app_id;
  return true;
}

} // namespace

void start() {
  // Create the optional SPRX catalog directory before the first UI scan.
  // Some PS5 runtimes report ENOTDIR for an absent O_DIRECTORY leaf.
  const auto prepared = prepare_catalog_directory();
  if (!prepared)
    LOG_WARN("[sprx-manager] startup catalog directory: %s", prepared.error.c_str());
  run("startup", true);
}
void reconcile() { run("reconcile", false); }

std::vector<onion::sprx::SprxInventoryEntry> inventory(std::string *error) {
  if (error) error->clear();
  Runtime &current = runtime();
  std::lock_guard<std::mutex> lock(current.mutex);
  // A catalog copied after startup must become visible without restarting the
  // game or daemon. Loading a catalog no longer dispatches any module.
  std::vector<onion::sprx::SprxCatalogIssue> issues;
  if (!current.manager.load_catalog(kCatalogPath, &issues)) {
    struct stat info{};
    // No catalog yet is an empty installation. Other failures must be visible.
    const bool absent = lstat(kCatalogPath, &info) != 0 && errno == ENOENT;
    if (!absent && error) {
      const auto &issue = issues.empty()
          ? onion::sprx::SprxCatalogIssue{0, {}, "plugin catalog could not be loaded"}
          : issues.front();
      *error = onion::ipc::plugin_diagnostic(
          (issue.line ? "Catalog line " + std::to_string(issue.line) + ": " : "Catalog: ") +
          issue.message);
    }
    if (!absent) return {};
  }
  auto entries = current.manager.inventory();
  const auto discovered = onion::sprx::discover_sprx_files(current.manager.catalog(), discovery_roots());
  if (!discovered.error.empty()) {
    LOG_WARN("[sprx-manager] inventory discovery failed: %s", discovered.error.c_str());
    if (error) *error = onion::ipc::plugin_diagnostic(discovered.error);
    return entries;
  }
  entries.insert(entries.end(), discovered.candidates.begin(), discovered.candidates.end());
  return entries;
}

onion::sprx::SprxOperationResult set_enabled(std::string_view id,
                                             bool enabled) {
  Runtime &current = runtime();
  std::lock_guard<std::mutex> lock(current.mutex);
  if (!current.manager.load_catalog(kCatalogPath) && !catalog_absent())
    return {false, "plugin catalog could not be loaded"};
  const auto catalog = current.manager.catalog();
  if (!catalog.find(id)) {
    if (!enabled) return {false, "unregistered plugin cannot be disabled"};
    const auto discovered = onion::sprx::discover_sprx_files(catalog, discovery_roots());
    if (!discovered.error.empty()) return {false, discovered.error};
    const onion::sprx::SprxInventoryEntry *candidate = nullptr;
    for (const auto &item : discovered.candidates) if (item.id == id) { candidate = &item; break; }
    if (!candidate) return {false, "plugin file is no longer available; refresh the list"};
    std::string error;
    if (!onion::sprx::valid_discovered_sprx(candidate->path, discovery_roots(), &error)) return {false, error};
    onion::sprx::SprxTarget target;
    if (!current.target.current(&target) || target.title_id.empty() || target.version.empty())
      return {false, "start the exact target game before registering this plugin"};
    const auto directory = prepare_catalog_directory();
    if (!directory) return directory;
    onion::sprx::SprxManifestEntry entry;
    entry.id = candidate->id; entry.path = candidate->path;
    entry.exact_title_ids = {target.title_id}; entry.game_version = target.version;
    entry.auto_start = false;
    const auto registered = current.manager.register_entry(entry);
    if (!registered) return registered;
  }
  return current.manager.set_enabled(id, enabled);
}

onion::sprx::SprxOperationResult remove(std::string_view id) {
  Runtime &current = runtime();
  std::lock_guard<std::mutex> lock(current.mutex);
  return current.manager.remove(id);
}

void on_big_app_started(pid_t pid, uint32_t app_id,
                        std::string_view title_id) {
  Runtime &current = runtime();
  std::lock_guard<std::mutex> lock(current.mutex);
  if (!is_current_target(current, pid, app_id, title_id))
    return;
  log_report("big-app-start", current.manager.reconcile());
}

void on_big_app_exited(pid_t pid, uint32_t app_id,
                       std::string_view title_id) {
  LOG_INFO("[sprx-manager] target exited pid=%d appid=%u tid=%.*s; no daemon-owned SPRX runtime or UI session to release",
           static_cast<int>(pid), app_id, static_cast<int>(title_id.size()),
           title_id.data());
}

void stop() {
  Runtime &current = runtime();
  std::lock_guard<std::mutex> lock(current.mutex);
  current.manager.stop();
  LOG_INFO("[sprx-manager] stopped target-bound modules");
}

} // namespace onion::daemon::sprx_plugins
