// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <set>
#include <string>

namespace onion::sprx {
// Injected operations make the same publication/cleanup policy testable with
// fault-injected host memory, without executing any supplied PRX code.
struct RaiBridgeOps {
  void *context;
  bool (*identity)(void *);
  uint64_t (*map)(void *, size_t);
  bool (*write)(void *, uint64_t, const void *, size_t);
  bool (*read)(void *, uint64_t, void *, size_t);
  bool (*protect)(void *, uint64_t, size_t, int);
  int (*protection)(void *, uint64_t, size_t);
  bool (*unmap)(void *, uint64_t, size_t);
};

class RaiBridgeLease final {
public:
  static constexpr size_t page = 0x4000, bytes = 2 * page;
  bool prepare(const RaiBridgeOps &ops, const void *code, size_t code_size,
               const void *control, size_t control_size) {
    if (address_ || published_ || !code || !code_size || code_size > page ||
        !control || !control_size || control_size > page || !ops.identity(ops.context)) return false;
    address_ = ops.map(ops.context, bytes);
    if (address_ < 0x10000 || address_ >= UINT64_C(0x800000000000) - bytes || (address_ & (page-1))) {
      // An invalid address was never accepted as owned target memory.
      address_ = 0; return false;
    }
    std::vector<unsigned char> observed(code_size > control_size ? code_size : control_size);
    if (!ops.identity(ops.context) ||
        !ops.write(ops.context, address_, code, code_size) ||
        !ops.write(ops.context, address_ + page, control, control_size) ||
        !ops.read(ops.context, address_, observed.data(), code_size) || memcmp(code, observed.data(), code_size) ||
        !ops.read(ops.context, address_ + page, observed.data(), control_size) || memcmp(control, observed.data(), control_size) ||
        !ops.protect(ops.context, address_, page, 5) ||
        !ops.protect(ops.context, address_ + page, page, 1) ||
        ops.protection(ops.context, address_, page) != 5 ||
        ops.protection(ops.context, address_ + page, page) != 1 || !ops.identity(ops.context)) return false;
    ready_ = true; return true;
  }
  uint64_t entry() const { return ready_ ? address_ : 0; }
  uint64_t control() const { return ready_ ? address_ + page : 0; }
  bool owns_mapping() const { return address_ != 0; }
  bool may_have_published() const { return published_; }
  // Call BEFORE the first enabled=1 write, even when that write can fail.
  void preserve_before_publish() { if (ready_) published_ = true; }
  void retain_after_uncertainty() { if (address_) published_ = true; }
  bool cleanup(const RaiBridgeOps &ops) {
    if (!address_ || published_) return true;
    if (!ops.identity(ops.context)) return false; // Never touch a reused PID.
    if (!ops.unmap(ops.context, address_, bytes)) return false;
    address_ = 0; ready_ = false; return true;
  }
private:
  uint64_t address_ = 0;
  bool ready_ = false, published_ = false;
};

struct RaiSessionOps {
  void *context = nullptr;
  int (*attach)(void *, int *) = nullptr;
  bool (*detach)(void *) = nullptr;
};

class RaiOwnedSession final {
public:
  void acquire(RaiSessionOps ops) {
    ops_ = ops;
    ready_ = ops_.attach && ops_.attach(ops_.context, &owned_) == 0;
  }
  bool ready() const { return ready_ && !closed_; }
  bool closed() const { return closed_; }
  bool resume_uncertain() const { return uncertain_; }
  bool close() {
    if (closed_) return !uncertain_;
    closed_ = true;
    // An attach/wait error can still transfer ownership. Explicitly detach
    // even when ready()==false, and never retry after an uncertain close.
    if (owned_ && (!ops_.detach || !ops_.detach(ops_.context))) {
      uncertain_ = true; ready_ = false; return false;
    }
    owned_ = 0; ready_ = false; return true;
  }
private:
  RaiSessionOps ops_{};
  int owned_ = 0;
  bool ready_ = false, closed_ = false, uncertain_ = false;
};

struct RaiCleanupState {
  bool cleanup_ok = true;
  bool resume_uncertain = false;
  void note_cleanup(bool confirmed) { if (!confirmed) cleanup_ok = false; }
  void note_resume(bool confirmed) {
    if (!confirmed) { cleanup_ok = false; resume_uncertain = true; }
  }
};

template<class Result, class Status>
Result rai_apply_cleanup_outcome(Result result, const RaiCleanupState &state,
                                 Status attach_failed, Status remote_error) {
  if (state.cleanup_ok) return result;
  result.cleanup_ok = false;
  if (state.resume_uncertain) {
    result.underlying_status = result.status; result.status = attach_failed;
  } else if (result.succeeded()) {
    result.underlying_status = result.status; result.status = remote_error;
  }
  return result;
}

class RaiGenerationGuard final {
public:
  void hold(const std::string &key) { held_.insert(key); }
  bool in_memory(const std::string &key) const { return held_.contains(key); }
  bool blocked(const std::string &key, int persistent_receipt) const {
    return in_memory(key) || persistent_receipt != 0;
  }
private:
  std::set<std::string> held_;
};

inline bool rai_finish_preparation(RaiBridgeLease &mapping, const RaiBridgeOps &operations,
                                    RaiOwnedSession &session, RaiCleanupState &state,
                                    bool prepared) {
  if (!prepared && mapping.owns_mapping()) {
    const bool cleaned = session.ready() && mapping.cleanup(operations);
    state.note_cleanup(cleaned);
    if (!cleaned) mapping.retain_after_uncertainty();
  }
  state.note_resume(session.close());
  if (!state.cleanup_ok) mapping.retain_after_uncertainty();
  return prepared && state.cleanup_ok;
}
} // namespace onion::sprx
