/* SPDX-License-Identifier: GPL-3.0-or-later
 * HyperHEN game-owned plugin worker. Workflow follows TacHen's native
 * module loader, adapted to the existing Toolbox/ptrace transport.
 */
#include "plugin-filesystem.h"
#include <onion/sprx_loader.hpp>
#include <onion/credentials_guard.h>
#include <onion/plugin_staging.hpp>
#include <onion/rai_bridge_lease.hpp>
#include "raidougie_compat.h"
#include "raidougie_syscall_bridge.h"
#include "raidougie_bridge_blob.h"
#include "game-runtime/game-lease.h"
#include "game-runtime/game-syscall.h"
extern "C" {
#include "game-runtime/game-inject.h"
#include "game-runtime/game-gate.h"
}
#include <onion/fs.h>
#include <onion/log.h>
#include <onion/pt.h>
#include <onion/system_tmp.h>
#include <onion/ucred.h>
#include "prx-image.h"
extern "C" {
#include "sha256.h"
extern const unsigned char hyperhen_plugin_worker_start[];
extern const unsigned char hyperhen_plugin_worker_end[];
}
#include <ps5/kernel.h>
#include <onion/proc_dmap.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

namespace onion::sprx {
namespace {
constexpr const char *kPluginStateRoot = "/data/HyperHEN/plugin-state-v1";
constexpr const char *kPreviousAttemptRoot = ONION_SYSTEM_TMP_ROOT "/plugin-modules-r50";
constexpr size_t kPage = 0x4000;
constexpr uint64_t kReceiptMagic = UINT64_C(0x4859505258330001);
enum { Intent=1, Started=2, Failed=3, NotDispatched=4 };
struct Receipt {
  uint64_t magic = kReceiptMagic;
  int64_t boot_seconds = 0, boot_microseconds = 0;
  int64_t seconds = 0, microseconds = 0;
  int32_t pid = -1;
  uint32_t state = Intent;
  int32_t handle = -1, initializer = -1, activation = -1;
  uint32_t reserved = 0;
  char name[256]{};
  uint64_t context = 0, load = 0, resolve = 0, control = 0, cookie = 0;
  char content_key[72]{};
  char staged_path[512]{};
  char metadata_hash[65]{};
};
bool boot_time(timeval &out) {
  int mib[] = {CTL_KERN, KERN_BOOTTIME}; size_t size = sizeof(out);
  return sysctl(mib, 2, &out, &size, nullptr, 0) == 0 && size == sizeof(out);
}
struct AttemptStore {
  std::string root, generation;
  int error = EIO;
};
AttemptStore build_attempt_store() {
  AttemptStore store; timeval boot{};
  if(!boot_time(boot)) return store;
  if(boot.tv_sec <= 0 || boot.tv_usec < 0 || boot.tv_usec >= 1000000) {
    store.error = EINVAL; return store;
  }
  char generation[64];
  const int size = snprintf(generation, sizeof(generation), "%lld-%lld",
    static_cast<long long>(boot.tv_sec), static_cast<long long>(boot.tv_usec));
  if(size < 0 || static_cast<size_t>(size) >= sizeof(generation)) {
    store.error = ENAMETOOLONG; return store;
  }
  store.generation = generation;
  store.root = std::string(kPluginStateRoot) + "/" + store.generation;
  store.error = 0;
  return store;
}
const AttemptStore &attempt_store() {
  // One immutable location for this daemon lifetime, including failure.
  // Never switch roots after a gate, receipt or in-memory hold is created.
  static const AttemptStore store = build_attempt_store();
  return store;
}
int state_directory(int parent, const char *name, mode_t mode, bool private_mode) {
  if(hhp_fs_mkdirat(parent, name, mode) && errno != EEXIST) return -1;
  int fd = hhp_fs_openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW, 0);
  if(fd < 0) return -1;
  struct stat st{};
  const int status = fstat(fd, &st);
  const int error = status ? (errno ? errno : EIO) :
    (!S_ISDIR(st.st_mode) || (st.st_uid != 0 && st.st_uid != getuid()) ||
     (private_mode && (st.st_mode & 0077))) ? EPERM : 0;
  if(error) { close(fd); errno = error; return -1; }
  return fd;
}
bool prepare_attempt_store() {
  const auto &store = attempt_store();
  if(store.error || store.root.empty() || store.generation.empty()) {
    errno = store.error ? store.error : EIO; return false;
  }
  int fd = open("/data", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if(fd < 0) return false;
  const char *parts[] = {"HyperHEN", "plugin-state-v1", store.generation.c_str()};
  for(unsigned i = 0; i < 3; ++i) {
    const int next = state_directory(fd, parts[i], i ? 0700 : 0755, i != 0);
    const int error = errno;
    close(fd); fd = next;
    if(fd < 0) { errno = error; return false; }
  }
  close(fd); return true;
}
std::string receipt_path(const GameIdentity &id, std::string_view name) {
  const auto &store = attempt_store();
  if(store.error || store.root.empty()) { errno = store.error ? store.error : EIO; return {}; }
  SHA256_CTX sha; unsigned char digest[32]; char hex[65]{};
  sha256_init(&sha); sha256_update(&sha, reinterpret_cast<const uint8_t *>(name.data()), name.size()); sha256_final(&sha, digest);
  for(unsigned i=0;i<32;++i) snprintf(hex+i*2, 3, "%02x", digest[i]);
  char result[256];
  const int size = snprintf(result, sizeof(result), "%s/%d-%lld-%lld-%s.state",
           store.root.c_str(), id.pid, static_cast<long long>(id.seconds),
           static_cast<long long>(id.microseconds), hex);
  if(size < 0 || static_cast<size_t>(size) >= sizeof(result)) { errno = ENAMETOOLONG; return {}; }
  return result;
}
int read_receipt(const GameIdentity &id, std::string_view name, Receipt &r) {
  const std::string path = receipt_path(id, name);
  if(path.empty()) return -1;
  int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  if(fd < 0) return errno == ENOENT ? 0 : -1;
  struct stat st{}; timeval boot{};
  const bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
    st.st_size == sizeof(r) && read(fd, &r, sizeof(r)) == sizeof(r) && boot_time(boot) &&
    r.magic == kReceiptMagic && r.pid == id.pid && r.seconds == id.seconds &&
    r.microseconds == id.microseconds && r.boot_seconds == boot.tv_sec &&
    r.boot_microseconds == boot.tv_usec && strnlen(r.name, sizeof(r.name)) == name.size() &&
    name == r.name && strnlen(r.metadata_hash,sizeof(r.metadata_hash)) == 64 &&
    (r.state == Intent || r.state == Started || r.state == Failed || r.state == NotDispatched);
  close(fd); return ok ? 1 : -1;
}
bool save_receipt(const GameIdentity &id, std::string_view name, const Receipt &r) {
  if(name.empty() || name.size() >= sizeof(r.name)) return false;
  if(!prepare_attempt_store()) return false;
  const std::string path = receipt_path(id, name);
  if(path.empty()) return false;
  Receipt stored = r;
  memset(stored.name, 0, sizeof(stored.name)); memcpy(stored.name, name.data(), name.size());
  return write_file_atomic(path.c_str(), &stored, sizeof(stored));
}
bool read_gate(const GameIdentity &id, uint64_t address, uint64_t cookie, TacGameGate &gate) {
  return address>=0x10000 && address<=UINT64_C(0x7fffffffffff)-sizeof(gate) &&
    same_game(id) && !onion_proc_copyout(id.pid,address,&gate,sizeof(gate)) &&
    gate.magic==TAC_GAME_GATE_MAGIC && gate.version==TAC_GAME_GATE_VERSION &&
    gate.bytes==sizeof(gate) && gate.cookie==cookie && gate.enabled==1 &&
    gate.loadStartModule && gate.threadCreate && gate.threadDetach && gate.dlsym && same_game(id);
}
bool refresh_completion(const GameIdentity &id, std::string_view name, Receipt &r) {
  if(r.state!=Intent || strnlen(r.content_key,sizeof(r.content_key))!=71 ||
     strncmp(r.content_key,"sha256:",7) || strnlen(r.staged_path,sizeof(r.staged_path))==sizeof(r.staged_path))return false;
  TacGameGate gate{};
  if(!read_gate(id,r.control,r.cookie,gate) || gate.loadStartModule!=r.load || gate.dlsym!=r.resolve)return false;
  const uint64_t first=r.control+offsetof(TacGameGate,modules);
  if(r.context<first || (r.context-first)%sizeof(TacGameModule) || (r.context-first)/sizeof(TacGameModule)>=TAC_GAME_MODULE_SLOTS)return false;
  const auto &slot=gate.modules[(r.context-first)/sizeof(TacGameModule)];
  if(strnlen(slot.path,sizeof(slot.path))==sizeof(slot.path) || strcmp(slot.path,r.staged_path) ||
    (slot.state!=TAC_MODULE_STARTED && slot.state!=TAC_MODULE_FAILED && slot.state!=TAC_MODULE_REJECTED))return false;
  std::string version,metadata;
  if(!game_version(id,version,metadata) || metadata!=r.metadata_hash)return false;
  r.handle=slot.handle;r.initializer=slot.startResult;r.activation=slot.activationResult;
  r.state=slot.state==TAC_MODULE_STARTED?Started:Failed;
  // A rejected thread creation never entered the initializer. Only this proven
  // pre-dispatch terminal outcome may be retried by another manual request.
  if(slot.state==TAC_MODULE_REJECTED && slot.phase==TAC_MODULE_PHASE_THREAD && slot.threadResult!=0 && !gate.busy)
    r.state=NotDispatched;
  if(!save_receipt(id,r.content_key,r) || !save_receipt(id,name,r)){r.state=Intent;return false;}
  LOG_INFO("[plugins] completion pid=%d module=%.*s phase=%u state=%u thread=%08x handle=%08x start=%08x activation=%08x",
    id.pid,static_cast<int>(name.size()),name.data(),slot.phase,slot.state,
    static_cast<unsigned>(slot.threadResult),static_cast<unsigned>(slot.handle),
    static_cast<unsigned>(slot.startResult),static_cast<unsigned>(slot.activationResult));
  return true;
}
bool receipt_pending(const GameIdentity &id,const Receipt &r){
  if(r.state!=Intent)return false;
  TacGameGate gate{};
  if(!read_gate(id,r.control,r.cookie,gate) || gate.loadStartModule!=r.load || gate.dlsym!=r.resolve)return false;
  const uint64_t first=r.control+offsetof(TacGameGate,modules);
  if(r.context<first || (r.context-first)%sizeof(TacGameModule) || (r.context-first)/sizeof(TacGameModule)>=TAC_GAME_MODULE_SLOTS)return false;
  const auto &slot=gate.modules[(r.context-first)/sizeof(TacGameModule)];
  return slot.enabled==1 && (slot.state==TAC_MODULE_PENDING || slot.state==TAC_MODULE_STARTING) &&
    strnlen(slot.path,sizeof(slot.path))<sizeof(slot.path) &&
    strnlen(r.staged_path,sizeof(r.staged_path))<sizeof(r.staged_path) && !strcmp(slot.path,r.staged_path);
}
bool clear_not_dispatched(const GameIdentity &id,std::string_view name,Receipt &r){
  if(r.state!=NotDispatched || !same_game(id))return false;
  const auto path=receipt_path(id,name);
  if(path.empty())return false;
  if(unlink(path.c_str()) && errno!=ENOENT)return false;
  // Remove only the receipt just read. An old alias may refer to content that
  // has since loaded successfully through a different alias. Its shared
  // content receipt must be re-read and reconciled, never deleted via alias.
  return true;
}
#ifndef HYPERHEN_PLUGIN_LEGACY_ROOT
#define HYPERHEN_PLUGIN_LEGACY_ROOT ONION_SYSTEM_TMP_ROOT "/plugin-modules"
#endif
bool previous_attempt_in_root(const GameIdentity &id,const char *root){
  DIR *dir=opendir(root);
  if(!dir)return errno!=ENOENT;
  char prefix[96];snprintf(prefix,sizeof(prefix),"%d-%lld-%lld-",id.pid,
    static_cast<long long>(id.seconds),static_cast<long long>(id.microseconds));
  const std::string gate_prefix=std::string("gate-")+prefix;
  bool found=false;unsigned count=0;
  for(;;){
    errno=0;dirent *entry=readdir(dir);
    if(!entry){found=errno!=0;break;}
    // Matching incomplete/temp files also hold this generation. Old state is
    // never migrated, overwritten or deleted when changing runtime storage.
    if(++count>16384 || !strncmp(entry->d_name,prefix,strlen(prefix)) ||
       !strncmp(entry->d_name,gate_prefix.c_str(),gate_prefix.size())){found=true;break;}
  }
  const int closed=closedir(dir);return found || closed!=0;
}
bool previous_runtime_attempt(const GameIdentity &id){
  return previous_attempt_in_root(id,HYPERHEN_PLUGIN_LEGACY_ROOT) ||
         previous_attempt_in_root(id,kPreviousAttemptRoot);
}
bool attempt_slot_available(const GameIdentity &id) {
  const auto &store=attempt_store();
  if(store.error || store.root.empty()){errno=store.error?store.error:EIO;return false;}
  DIR *dir = opendir(store.root.c_str());
  if(!dir) return errno == ENOENT;
  char prefix[96]; snprintf(prefix, sizeof(prefix), "%d-%lld-%lld-", id.pid,
    static_cast<long long>(id.seconds), static_cast<long long>(id.microseconds));
  unsigned total = 0, inspected = 0;
  for(;;) {
    errno=0;dirent *entry=readdir(dir);
    if(!entry){const int error=errno;const int closed=closedir(dir);return !error && !closed && total<8;}
    if(++inspected > 16384) { closedir(dir); return false; }
    if(strncmp(entry->d_name, prefix, strlen(prefix))) continue;
    int fd = hhp_fs_openat(dirfd(dir), entry->d_name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK, 0);
    Receipt r; struct stat st{};
    bool ok = fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
       st.st_size == sizeof(r) && read(fd, &r, sizeof(r)) == sizeof(r) &&
       r.magic == kReceiptMagic && strnlen(r.name, sizeof(r.name)) < sizeof(r.name);
    if(fd >= 0) close(fd);
    if(!ok) { closedir(dir); return false; }
    if(!strncmp(r.name, "sha256:", 7)) ++total;
  }
}
LoadResult result_for(LoadStatus status, pid_t pid, bool clean = true) {
  LoadResult r; r.status = r.underlying_status = status; r.pid = pid; r.cleanup_ok = clean; return r;
}
class Session {
  HhpSelfCredentialsGuard credentials_;
  pid_t pid_; uintptr_t old_=0; int owned_=0; bool ready_=false;
public:
  explicit Session(pid_t pid):pid_(pid){if(!credentials_.ready())return;old_=set_ucred_to_ptrace();ready_=old_ && hhp_plugin_attach_owned(pid_,&owned_)==0;}
  bool ready()const{return ready_;}
  bool detach(){if(!owned_)return true;if(!hhp_plugin_transport_can_resume(pid_)||pt_detach(pid_,0))return false;owned_=0;ready_=false;return true;}
  LoadResult finish(LoadResult r){
    if(!detach()){r.underlying_status=r.status;r.status=LoadStatus::AttachFailed;r.cleanup_ok=false;}
    return r;
  }
  ~Session(){detach();if(old_)(void)set_proc_authid(getpid(),old_);}
};

class RaiSession {
  HhpSelfCredentialsGuard credentials_;
  pid_t pid_; uintptr_t old_=0;
  RaiOwnedSession state_;
public:
  explicit RaiSession(pid_t pid):pid_(pid){
    if(!credentials_.ready())return;
    old_=set_ucred_to_ptrace();
    if(old_)state_.acquire({this,
      [](void *p,int *owned){return hhp_plugin_attach_owned(static_cast<RaiSession*>(p)->pid_,owned);},
      [](void *p){auto &self=*static_cast<RaiSession*>(p);return hhp_plugin_transport_can_resume(self.pid_)&&pt_detach(self.pid_,0)==0;}});
  }
  bool ready()const{return state_.ready();}
  RaiOwnedSession &state(){return state_;}
  bool detach(){return state_.close();}
  LoadResult finish(LoadResult r){
    if(!detach()){r.underlying_status=r.status;r.status=LoadStatus::AttachFailed;r.cleanup_ok=false;}
    return r;
  }
  ~RaiSession(){
    if(!state_.closed()&&!state_.close())LOG_ERROR("[plugins] unfinalized session resume uncertain pid=%d",pid_);
    if(old_)(void)set_proc_authid(getpid(),old_);
  }
};

// Retain the canonical session behavior with the shared credential guard.
class LaunchSession final {
  std::optional<Session> original_;
  std::optional<RaiSession> rai_;
public:
  LaunchSession(pid_t pid,bool rai){if(rai)rai_.emplace(pid);else original_.emplace(pid);}
  bool ready()const{return rai_?rai_->ready():original_->ready();}
  bool detach(){return rai_?rai_->detach():original_->detach();}
  LoadResult finish(LoadResult result){return rai_?rai_->finish(result):original_->finish(result);}
};

// Access is serialized by PtraceSprxRuntime::load's existing operation mutex.
RaiGenerationGuard &rai_uncertain_generations(){static RaiGenerationGuard values;return values;}
std::string rai_hold_key(const GameIdentity &id){return receipt_path(id,"@Rai-uncertain");}
bool rai_generation_held(const GameIdentity &id){
  if(rai_uncertain_generations().in_memory(rai_hold_key(id)))return true;
  Receipt record;
  return rai_uncertain_generations().blocked(rai_hold_key(id),read_receipt(id,"@Rai-uncertain",record));
}
bool persist_rai_hold(const GameIdentity &id,std::string_view name,std::string_view metadata){
  // Keep an in-memory hold even if full/unwritable storage prevents persistence.
  // A durable Failed record must never be normalized into NotDispatched.
  rai_uncertain_generations().hold(rai_hold_key(id));
  timeval boot{};
  if(metadata.size()!=64 || !boot_time(boot))return false;
  Receipt record;record.pid=id.pid;record.seconds=id.seconds;record.microseconds=id.microseconds;
  record.boot_seconds=boot.tv_sec;record.boot_microseconds=boot.tv_usec;record.state=Failed;
  memcpy(record.metadata_hash,metadata.data(),metadata.size());
  const std::string source_key=std::string("sha256:")+HHP_RAIDOUGIE_SHA;
  memcpy(record.content_key,source_key.c_str(),source_key.size()+1);
  const bool generation=save_receipt(id,"@Rai-uncertain",record);
  const bool source=save_receipt(id,source_key,record);
  const bool alias=save_receipt(id,name,record);
  return generation&&source&&alias;
}

class RaiBridge final {
  GameIdentity id_;
  std::string metadata_;
  RaiBridgeLease lease_;
  RaiStageAdapter adapter_;
  RaiCleanupState cleanup_;
  std::string name_;
  bool finalized_=false;
  bool identity() const { return same_game(id_); }
  RaiBridgeOps ops() {
    return {this,
      [](void *p){return static_cast<RaiBridge *>(p)->identity();},
      [](void *p,size_t length)->uint64_t {
        auto &self=*static_cast<RaiBridge *>(p);
        if(!self.identity()||!hhp_plugin_transport_healthy(self.id_.pid))return 0;
        LOG_INFO("[plugins] Rai bridge mmap begin pid=%d bytes=%zu",self.id_.pid,length);
        const auto address=hhp_plugin_mmap(self.id_.pid,0,length,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        LOG_INFO("[plugins] Rai bridge mmap end pid=%d address=%llx",self.id_.pid,(unsigned long long)address);
        return address>0?static_cast<uint64_t>(address):0;
      },
      [](void *p,uint64_t address,const void *bytes,size_t size){
        auto &self=*static_cast<RaiBridge *>(p);LOG_INFO("[plugins] Rai bridge copyin begin pid=%d bytes=%zu",self.id_.pid,size);
        const bool ok=self.identity()&&hhp_plugin_transport_healthy(self.id_.pid)&&!pt_copyin(self.id_.pid,bytes,address,size);
        LOG_INFO("[plugins] Rai bridge copyin end pid=%d ok=%d",self.id_.pid,ok);return ok;
      },
      [](void *p,uint64_t address,void *bytes,size_t size){
        auto &self=*static_cast<RaiBridge *>(p);LOG_INFO("[plugins] Rai bridge copyout begin pid=%d bytes=%zu",self.id_.pid,size);
        const bool ok=self.identity()&&hhp_plugin_transport_healthy(self.id_.pid)&&!pt_copyout(self.id_.pid,address,bytes,size);
        LOG_INFO("[plugins] Rai bridge copyout end pid=%d ok=%d",self.id_.pid,ok);return ok;
      },
      [](void *p,uint64_t address,size_t size,int protection){
        auto &self=*static_cast<RaiBridge *>(p);LOG_INFO("[plugins] Rai bridge protect begin pid=%d bytes=%zu prot=%d",self.id_.pid,size,protection);
        // kernel_mprotect edits an entire VM entry; it does not split the
        // newly allocated code/data arena or update its page tables. Like
        // etaHEN PortSealGameArena, use the target's real mprotect here.
        const bool ok=self.identity()&&hhp_plugin_transport_healthy(self.id_.pid)&&!hhp_plugin_mprotect(self.id_.pid,address,size,protection);
        LOG_INFO("[plugins] Rai bridge protect end pid=%d ok=%d",self.id_.pid,ok);return ok;
      },
      [](void *p,uint64_t address,size_t size){
        auto &self=*static_cast<RaiBridge *>(p);
        const bool owned=self.identity()&&hhp_plugin_transport_healthy(self.id_.pid);
        const int protection=owned?kernel_get_vmem_protection(self.id_.pid,address,size):-1;
        LOG_INFO("[plugins] Rai bridge query pid=%d address=%llx bytes=%zu protection=%d owned=%d",self.id_.pid,(unsigned long long)address,size,protection,owned);
        return protection;
      },
      [](void *p,uint64_t address,size_t size){
        auto &self=*static_cast<RaiBridge *>(p);return self.identity()&&hhp_plugin_transport_healthy(self.id_.pid)&&!hhp_plugin_munmap(self.id_.pid,address,size);
      }};
  }
  bool prepare(const GameIdentity &id,std::vector<unsigned char> &bytes,std::string &error) {
    static_assert(PROT_READ==1 && PROT_WRITE==2 && PROT_EXEC==4);
    if(id.pid!=id_.pid || id.seconds!=id_.seconds || id.microseconds!=id_.microseconds ||
       id.title!=id_.title || !identity() || hhp_raidougie_admit(bytes.data(),bytes.size())) {
      error="Rai source or game identity is not admitted";return false;
    }
    Receipt held_source;
    const int source_record=read_receipt(id_,std::string("sha256:")+HHP_RAIDOUGIE_SHA,held_source);
    if(rai_generation_held(id_) || source_record<0 ||
       (source_record==1&&held_source.state==Failed)){
      cleanup_.note_cleanup(false);error="Rai has a held outcome for this game generation; restart the game";return false;
    }
    const size_t process_info_size=sizeof(hhp_rai_bridge_bytes);
    const size_t syscall_offset=(process_info_size+15u)&~size_t(15u);
    const size_t code_size=syscall_offset+HHP_RAI_SYSCALL_BRIDGE_BYTES;
    if(!process_info_size || code_size>0x1000){error="Rai bridge image is invalid";return false;}
    std::vector<unsigned char> bridge_code(code_size,0x90);
    memcpy(bridge_code.data(),hhp_rai_bridge_bytes,process_info_size);
    RaiGameMetadata metadata;
    if(!rai_game_metadata(id_,metadata,error) || metadata.metadata_hash!=metadata_ || !identity()) {
      if(error.empty())error="Rai metadata changed before stopping the game";
      return false;
    }
    LOG_INFO("[plugins] runtime revision=R72 parent=R71 pid=%d",id_.pid);
    LOG_INFO("[plugins] Rai attach begin pid=%d",id_.pid);
    RaiSession stopped(id_.pid);
    LOG_INFO("[plugins] Rai attach end pid=%d ready=%d",id_.pid,stopped.ready());
    uint64_t nonce=0;
    const bool prepared=[&]() -> bool {
    if(!stopped.ready()){error="Rai owned session not ready after attach";return false;}
    if(!identity()){error="Rai game identity check failed after attach";return false;}
    RaiGameSnapshot snapshot;
    LOG_INFO("[plugins] Rai process snapshot begin pid=%d",id_.pid);
    if(!rai_game_process_snapshot(id_,metadata,snapshot,error)) {
      if(error.empty())error="Rai process changed before bridge preparation";
      return false;
    }
    LOG_INFO("[plugins] Rai process snapshot end pid=%d base=%llx",id_.pid,(unsigned long long)snapshot.base);
    HhpRaiInfo info{};info.pid=snapshot.pid;info.base=snapshot.base;
    const auto copy=[](char *target,size_t capacity,const std::string &text){
      if(text.empty() || text.size()>=capacity)return false;
      memcpy(target,text.c_str(),text.size()+1);return true;
    };
    if(!copy(info.name,sizeof(info.name),snapshot.name) || !copy(info.path,sizeof(info.path),snapshot.path) ||
       !copy(info.title,sizeof(info.title),metadata.title) || !copy(info.content,sizeof(info.content),metadata.content) ||
       !copy(info.version,sizeof(info.version),metadata.version)){error="Rai snapshot field exceeds wire bounds";return false;}
    do{arc4random_buf(&nonce,sizeof(nonce));}while(!nonce);
    HhpRaiControl control{};
    if(hhp_raidougie_control_init(&control,nonce,&info)){error="Rai snapshot is invalid";return false;}
    const intptr_t native_gate=hhp_plugin_native_syscall_gate(id_.pid);
    if(native_gate<=0 || !identity() || !hhp_plugin_transport_healthy(id_.pid) ||
       hhp_raidougie_syscall_bridge(bridge_code.data()+syscall_offset,HHP_RAI_SYSCALL_BRIDGE_BYTES,(uint64_t)native_gate)) {
      error="Rai native syscall return sequence is unavailable";return false;
    }
    const auto operations=ops();
    LOG_INFO("[plugins] Rai bridge lease begin pid=%d",id_.pid);
    const bool lease_ready=lease_.prepare(operations,bridge_code.data(),code_size,&control,sizeof(control));
    LOG_INFO("[plugins] Rai bridge lease end pid=%d ready=%d transport=%d",id_.pid,lease_ready,hhp_plugin_transport_healthy(id_.pid));
    if(!hhp_plugin_transport_healthy(id_.pid)){
      cleanup_.note_cleanup(false);
      error="Rai remote-syscall outcome is uncertain; restart the game before retrying";return false;
    }
    if(!lease_ready){error="Rai bridge preparation failed";return false;}
    if(hhp_raidougie_patch(bytes.data(),bytes.size(),lease_.entry(),lease_.control(),lease_.entry()+syscall_offset)) {
      error="Rai exact patch failed";return false;
    }
    return true;
    }();
    if(!rai_finish_preparation(lease_,ops(),stopped.state(),cleanup_,prepared)) {
      if(!cleanup_.cleanup_ok)error="Rai cleanup or game resume could not be confirmed; restart the game";
      return false;
    }
    adapter_.nonce=nonce;
    LOG_INFO("[plugins] Rai snapshot prepared pid=%d generation=%lld:%lld bridge=%llx control=%llx nonce=%llx",
      id_.pid,static_cast<long long>(id_.seconds),static_cast<long long>(id_.microseconds),
      static_cast<unsigned long long>(lease_.entry()),static_cast<unsigned long long>(lease_.control()),
      static_cast<unsigned long long>(nonce));
    return true;
  }
public:
  RaiBridge(const GameIdentity &id,std::string metadata,std::string_view name):id_(id),metadata_(std::move(metadata)),name_(name) {
    adapter_.context=this;
    adapter_.prepare=[](void *p,const GameIdentity &identity,std::vector<unsigned char> &bytes,std::string &error){
      return static_cast<RaiBridge *>(p)->prepare(identity,bytes,error);
    };
  }
  RaiStageAdapter *adapter(){return &adapter_;}
  bool adapted()const{return adapter_.adapted;}
  const std::string &source_hash()const{return adapter_.source_sha256;}
  void preserve_before_publish(){if(adapter_.adapted)lease_.preserve_before_publish();}
  LoadResult finish(LoadResult result);
  ~RaiBridge(){
    if(!finalized_){
      lease_.retain_after_uncertainty();
      LOG_ERROR("[plugins] Rai finalization was omitted pid=%d; mapping retained",id_.pid);
    }
  }
};

LoadResult RaiBridge::finish(LoadResult result){
  if(finalized_)return result;
  finalized_=true;
  if(!hhp_raidougie_supported(adapter_.source_sha256.c_str()))return result;
  if(!result.cleanup_ok){
    cleanup_.note_cleanup(false);
    if(result.status==LoadStatus::AttachFailed)cleanup_.note_resume(false);
    lease_.retain_after_uncertainty();
  }
  if(lease_.owns_mapping()&&!lease_.may_have_published()){
    if(!cleanup_.cleanup_ok||!identity()){
      cleanup_.note_cleanup(false);lease_.retain_after_uncertainty();
    }else{
      LOG_INFO("[plugins] Rai attach begin pid=%d",id_.pid);
    RaiSession stopped(id_.pid);
    LOG_INFO("[plugins] Rai attach end pid=%d ready=%d",id_.pid,stopped.ready());
      const bool unmapped=stopped.ready()&&lease_.cleanup(ops());
      cleanup_.note_cleanup(unmapped);
      cleanup_.note_resume(stopped.detach());
      if(!cleanup_.cleanup_ok)lease_.retain_after_uncertainty();
    }
  }
  if(!cleanup_.cleanup_ok){
    const bool held=persist_rai_hold(id_,name_,metadata_);
    LOG_ERROR("[plugins] Rai outcome held pid=%d cleanup=0 resume-confirmed=%d durable-hold=%d; restart game before any retry",
      id_.pid,cleanup_.resume_uncertain?0:1,held?1:0);
    if(!held)LOG_ERROR("[plugins] Rai hold persistence FAILED pid=%d; in-memory generation guard remains active",id_.pid);
  }
  return rai_apply_cleanup_outcome(result,cleanup_,LoadStatus::AttachFailed,LoadStatus::RemoteError);
}
struct GateReceipt {
  uint64_t magic=UINT64_C(0x4848504741544501),control=0,arena=0,cookie=0;
  int64_t boot_seconds=0,boot_microseconds=0,seconds=0,microseconds=0;
  int32_t pid=-1;uint32_t state=Intent;
  intptr_t rootdir=0,jaildir=0;
};
std::string gate_path(const GameIdentity &id){auto p=receipt_path(id,"@game-callback");if(p.empty())return {};p.insert(p.find_last_of('/')+1,"gate-");return p;}
bool gate_identity(void *value){return same_game(*static_cast<GameIdentity*>(value));}
int identity_callback(void *value){return gate_identity(value)?1:0;}
bool acquire_gate(const GameIdentity &id,GateReceipt &record,TacGameGate &gate){
  const auto path=gate_path(id);timeval boot{};
  if(path.empty() || !boot_time(boot))return false;
  int fd=open(path.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
  if(fd>=0){
    struct stat st{};bool ok=fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_size==sizeof(record) &&
      read(fd,&record,sizeof(record))==sizeof(record);close(fd);
    ok=ok && record.magic==UINT64_C(0x4848504741544501) && record.pid==id.pid &&
      record.seconds==id.seconds && record.microseconds==id.microseconds &&
      record.rootdir==id.rootdir && record.jaildir==id.jaildir &&
      record.boot_seconds==boot.tv_sec && record.boot_microseconds==boot.tv_usec && record.state==Started;
    return ok && read_gate(id,record.control,record.cookie,gate);
  }
  if(errno!=ENOENT)return false;
  if(!prepare_attempt_store())return false;
  record={};record.pid=id.pid;record.seconds=id.seconds;record.microseconds=id.microseconds;
  record.rootdir=id.rootdir;record.jaildir=id.jaildir;
  record.boot_seconds=boot.tv_sec;record.boot_microseconds=boot.tv_usec;
  do{arc4random_buf(&record.cookie,sizeof(record.cookie));}while(!record.cookie);
  if(!write_file_atomic(path.c_str(),&record,sizeof(record)))return false;
  HhpSelfCredentialsGuard credentials;
  if(!credentials.ready()){(void)unlink(path.c_str());return false;}
  uintptr_t previous=set_ucred_to_ptrace();
  if(!previous){(void)unlink(path.c_str());return false;}
  TacGameInjection injection{};GameIdentity identity=id;
  const int result=tac_game_inject(id.pid,record.cookie,identity_callback,&identity,&injection);
  (void)set_proc_authid(getpid(),previous);
  LOG_INFO("[plugins] game callback pid=%d result=%d installed=%d resumed=%d: %s",id.pid,result,injection.installed,injection.resumed,injection.message);
  if(result || !injection.installed || !injection.resumed){
    if(result>0 && !injection.uncertain && injection.resumed && !injection.arena && same_game(id))(void)unlink(path.c_str());
    return false;
  }
  record.control=injection.control;record.arena=injection.arena;record.state=Started;
  return write_file_atomic(path.c_str(),&record,sizeof(record)) && read_gate(id,record.control,record.cookie,gate);
}
uint32_t host_sdk(pid_t pid) {
  intptr_t scratch = hhp_plugin_mmap(pid, 0, kPage, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if(scratch <= 0) return 0;
  uint64_t address = 0, size = 0; unsigned char param[32]{}; uint32_t sdk = 0;
  const uint64_t arguments[6]={(uint64_t)scratch,(uint64_t)(scratch+8),0,0,0,0};
  if(!hhp_plugin_syscall6(pid, 598, arguments) &&
     !pt_copyout(pid, scratch, &address, 8) && !pt_copyout(pid, scratch+8, &size, 8) &&
     address >= 0x10000 && address <= UINT64_C(0x7fffffffffff)-32 && size >= 32 && size <= 4096 &&
     !pt_copyout(pid, address, param, sizeof(param))) sdk = hhp_ps5_process_sdk(param, sizeof(param));
  (void)hhp_plugin_munmap(pid, scratch, kPage); return sdk;
}
}

bool PtraceSprxRuntime::find_loaded(pid_t pid, std::string_view name, ModuleInfo *out) const noexcept {
  if(!out || name.empty() || name.size() >= 256) return false;
  if(!prepare_attempt_store()) return false;
  GameIdentity id; Receipt r;
  if(!capture_game(pid, id) || read_receipt(id, name, r) != 1) return false;
  std::string version,metadata;
  if(!game_version(id,version,metadata) || metadata != r.metadata_hash) return false;
  (void)refresh_completion(id,name,r);
  if(r.state != Started || r.handle < 0 || r.initializer < 0 || r.activation < 0 || !same_game(id)) return false;
  out->handle = static_cast<uint32_t>(r.handle);
  return true;
}

LoadResult PtraceSprxRuntime::load(pid_t pid, std::string_view path,
                                  std::string_view name, const LoadOptions &options) noexcept {
  static std::mutex mutex; std::lock_guard<std::mutex> lock(mutex);
  if(pid <= 1 || path.empty() || name.empty() || name.size() >= 256 ||
     !options.timeout_ms || !options.poll_ms || options.poll_ms > options.timeout_ms)
    return result_for(LoadStatus::InvalidArgument, pid);
  if(!prepare_attempt_store()) {
    const int error=errno;
    LOG_ERROR("[plugins] private state store unavailable path=%s errno=%d",attempt_store().root.c_str(),error);
    return result_for(LoadStatus::WriteFailed,pid,true);
  }
  GameIdentity id; Receipt receipt;
  if(!capture_game(pid, id)) return result_for(LoadStatus::TargetNotFound, pid);
  hhp_plugin_transport_begin(pid,id.seconds,id.microseconds);
  if(!hhp_plugin_transport_healthy(pid)){
    LOG_ERROR("[plugins] uncertain remote-syscall transport prevents reuse pid=%d",pid);
    return result_for(LoadStatus::RemoteError,pid,false);
  }
  if(rai_generation_held(id)){
    LOG_ERROR("[plugins] held Rai outcome prevents dispatch pid=%d; restart the game",pid);
    return result_for(LoadStatus::RemoteError,pid,false);
  }
  if(previous_runtime_attempt(id)){
    LOG_WARN("[plugins] previous runtime attempt for this game generation; restart game before loading modules");
    return result_for(LoadStatus::PolicyDenied,pid,false);
  }
  std::string version, metadata_hash;
  if(!game_version(id,version,metadata_hash) ||
     (!options.game_version.empty() && version != options.game_version))
    return result_for(LoadStatus::PolicyDenied,pid);
  int prior = read_receipt(id, name, receipt);
  if(prior==1){(void)refresh_completion(id,name,receipt);if(receipt.state==NotDispatched && clear_not_dispatched(id,name,receipt))prior=0;}
  if(prior != 0) {
    if(prior == 1) (void)refresh_completion(id,name,receipt);
    if(prior == 1 && metadata_hash == receipt.metadata_hash && receipt.state == Started && receipt.handle >= 0 &&
       receipt.initializer >= 0 && receipt.activation >= 0) {
      LoadResult r = result_for(LoadStatus::AlreadyLoaded, pid);
      r.module_handle = static_cast<uint32_t>(receipt.handle); return r;
    }
    if(prior==1 && metadata_hash==receipt.metadata_hash && receipt_pending(id,receipt))return result_for(LoadStatus::Pending,pid,true);
    LOG_WARN("[plugins] prior module attempt held pid=%d name=%.*s; restart game before retry", pid, static_cast<int>(name.size()), name.data());
    return result_for(LoadStatus::RemoteError, pid, false);
  }
  uint32_t sdk=0;
  // PS4 modules do not use the PS5 development-SDK metadata adaptation.
  if(!id.title.starts_with("CUSA")) {
    Session session(pid);
    if(!session.ready()) return session.finish(result_for(LoadStatus::AttachFailed, pid));
    sdk=host_sdk(pid);
    if(!hhp_plugin_transport_healthy(pid))return session.finish(result_for(LoadStatus::RemoteError,pid,false));
    if(!session.detach()) return session.finish(result_for(LoadStatus::AttachFailed, pid, false));
  }
  RaiBridge rai(id,metadata_hash,name);
  std::string staged, hash, error;
  if(!stage_module(id, path, sdk, staged, hash, error,rai.adapter())) {
    LOG_ERROR("[plugins] stage failed pid=%d: %s", pid, error.c_str());
    return rai.finish(result_for(LoadStatus::AccessPrepareFailed, pid));
  }
  // Address-bearing patches must not defeat the cross-alias replay guard.
  // The two verified Rai containers share one logical content identity.
  // SELF and its byte-identical extracted ELF represent one module identity.
  // Their source hashes differ, but must share the same per-game replay guard.
  const std::string content_key = "sha256:" + (rai.adapted()?std::string(HHP_RAIDOUGIE_SHA):hash);
  int content_prior = read_receipt(id, content_key, receipt);
  if(content_prior==1){(void)refresh_completion(id,content_key,receipt);if(receipt.state==NotDispatched && clear_not_dispatched(id,content_key,receipt))content_prior=0;}
  if(content_prior != 0) {
    if(content_prior == 1) (void)refresh_completion(id,content_key,receipt);
    if(content_prior == 1 && metadata_hash == receipt.metadata_hash && receipt.state == Started && receipt.handle >= 0 &&
       receipt.initializer >= 0 && receipt.activation >= 0 && save_receipt(id, name, receipt)) {
      LoadResult r = result_for(LoadStatus::AlreadyLoaded, pid);
      r.module_handle = static_cast<uint32_t>(receipt.handle); return rai.finish(r);
    }
    if(content_prior==1 && metadata_hash==receipt.metadata_hash && receipt_pending(id,receipt))return rai.finish(result_for(LoadStatus::Pending,pid,true));
    return rai.finish(result_for(LoadStatus::RemoteError, pid, false));
  }
  if(!attempt_slot_available(id)) return rai.finish(result_for(LoadStatus::RemoteError, pid, false));
  GateReceipt gate_record;TacGameGate gate{};
  if(!acquire_gate(id,gate_record,gate))return rai.finish(result_for(LoadStatus::RemoteError,pid,false));
  if(gate.busy)return rai.finish(result_for(LoadStatus::RemoteError,pid,true));
  unsigned slot_index=TAC_GAME_MODULE_SLOTS;
  for(unsigned i=0;i<TAC_GAME_MODULE_SLOTS;i++)if(!gate.modules[i].enabled && gate.modules[i].state==TAC_MODULE_PENDING){slot_index=i;break;}
  // A confirmed failure to create a thread can reclaim its slot while stopped;
  // STARTING/FAILED/STARTED and unknown outcomes are never recycled.
  if(slot_index==TAC_GAME_MODULE_SLOTS)for(unsigned i=0;i<TAC_GAME_MODULE_SLOTS;i++){
    const auto &candidate=gate.modules[i];
    if(candidate.state==TAC_MODULE_REJECTED && candidate.phase==TAC_MODULE_PHASE_THREAD && candidate.threadResult){slot_index=i;break;}
  }
  if(slot_index==TAC_GAME_MODULE_SLOTS)return rai.finish(result_for(LoadStatus::RemoteError,pid,true));
  // The running game's sandbox may be unavailable while ptrace owns a stopped
  // process. Recheck the exact SFO digest while the game is running, then hold
  // that snapshot through the launch lease and recheck process identity below.
  std::string current_version,current_hash;
  if(!game_version(id,current_version,current_hash) || current_hash!=metadata_hash ||
     current_version!=version || !same_game(id))
    return rai.finish(result_for(LoadStatus::PolicyDenied,pid,true));
  LaunchSession launch(pid,rai.adapted());
  if(!launch.ready() || !same_game(id))return rai.finish(launch.finish(result_for(LoadStatus::AttachFailed,pid,true)));
  TacGameGate stopped{};
  if(!read_gate(id,gate_record.control,gate_record.cookie,stopped) || stopped.busy ||
     memcmp(&stopped.modules[slot_index],&gate.modules[slot_index],sizeof(TacGameModule)))
    return rai.finish(launch.finish(result_for(LoadStatus::RemoteError,pid,true)));
  if(!same_game(id))
    return rai.finish(launch.finish(result_for(LoadStatus::PolicyDenied,pid,true)));
  TacGameModule slot{};slot.handle=-1;
  if(staged.size()>=sizeof(slot.path))return rai.finish(launch.finish(result_for(LoadStatus::InvalidPath,pid,true)));
  memcpy(slot.path,staged.c_str(),staged.size()+1);
  const uint64_t address=gate_record.control+offsetof(TacGameGate,modules)+slot_index*sizeof(TacGameModule);
  uint32_t disabled=0;
  if(pt_copyin(pid,&disabled,address+offsetof(TacGameModule,enabled),sizeof(disabled)))
    return rai.finish(launch.finish(result_for(LoadStatus::WriteFailed,pid,false)));
  if(pt_copyin(pid,&slot,address,sizeof(slot)))return rai.finish(launch.finish(result_for(LoadStatus::WriteFailed,pid,true)));
  timeval boot{};if(!boot_time(boot)||!same_game(id))return rai.finish(launch.finish(result_for(LoadStatus::TargetExited,pid,true)));
  receipt={};receipt.pid=pid;receipt.seconds=id.seconds;receipt.microseconds=id.microseconds;
  receipt.boot_seconds=boot.tv_sec;receipt.boot_microseconds=boot.tv_usec;
  receipt.context=address;receipt.control=gate_record.control;receipt.cookie=gate_record.cookie;
  receipt.load=gate.loadStartModule;receipt.resolve=gate.dlsym;
  memcpy(receipt.name,name.data(),name.size());
  memcpy(receipt.content_key,content_key.c_str(),content_key.size()+1);
  memcpy(receipt.staged_path,staged.c_str(),staged.size()+1);
  memcpy(receipt.metadata_hash,metadata_hash.c_str(),65);
  // No callback can consume a disabled slot. Persist both identities before
  // publishing enabled=1; only publication or later uncertainty is held.
  if(!save_receipt(id,content_key,receipt)||!save_receipt(id,name,receipt)){
    receipt.state=NotDispatched;(void)save_receipt(id,content_key,receipt);(void)save_receipt(id,name,receipt);
    return rai.finish(launch.finish(result_for(LoadStatus::WriteFailed,pid,true)));
  }
  uint32_t enabled=1;
  rai.preserve_before_publish();
  if(pt_copyin(pid,&enabled,address+offsetof(TacGameModule,enabled),sizeof(enabled)))
    return rai.finish(launch.finish(result_for(LoadStatus::WriteFailed,pid,false)));
  if(!launch.detach())return rai.finish(launch.finish(result_for(LoadStatus::AttachFailed,pid,false)));
  LOG_INFO("[plugins] module queued pid=%d slot=%u; waiting for game callback",pid,slot_index);
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(std::min<uint32_t>(options.timeout_ms,250));
  bool finished=false;
  while(std::chrono::steady_clock::now()<deadline && same_game(id)){
    if(refresh_completion(id,name,receipt)){finished=true;break;}
    usleep(std::min<uint32_t>(options.poll_ms,1000)*1000);
  }
  if(!finished){
    TacGameGate observed{};
    if(read_gate(id,gate_record.control,gate_record.cookie,observed)){
      const auto &state=observed.modules[slot_index];
      LOG_WARN("[plugins] pending pid=%d slot=%u callbacks=%llu/%llu phase=%u state=%u thread=%d; no duplicate dispatch",pid,slot_index,
        static_cast<unsigned long long>(observed.callbacks[0]),static_cast<unsigned long long>(observed.callbacks[1]),state.phase,state.state,state.threadResult);
    }
    if(receipt_pending(id,receipt))return rai.finish(result_for(LoadStatus::Pending,pid,true));
    return rai.finish(result_for(same_game(id)?LoadStatus::Timeout:LoadStatus::TargetExited,pid,false));
  }
  const bool success=receipt.state==Started && receipt.handle>=0 && receipt.initializer>=0 && receipt.activation>=0;
  LOG_INFO("[plugins] initialized pid=%d module=%.*s handle=%08x start=%08x activation=%08x state=%u",pid,
    static_cast<int>(name.size()),name.data(),static_cast<unsigned>(receipt.handle),static_cast<unsigned>(receipt.initializer),
    static_cast<unsigned>(receipt.activation),receipt.state);
  LoadResult result=result_for(success?LoadStatus::Loaded:receipt.state==NotDispatched?LoadStatus::ThreadCreateFailed:LoadStatus::RemoteError,pid,success||receipt.state==NotDispatched);
  if(success)result.module_handle=static_cast<uint32_t>(receipt.handle);
  result.remote_result=receipt.handle<0?receipt.handle:receipt.initializer;
  return rai.finish(result);
}
}
