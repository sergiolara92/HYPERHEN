/* SPDX-License-Identifier: GPL-3.0-or-later
 * HyperHEN game-owned plugin worker. Workflow follows TacHen's native
 * module loader, adapted to the existing Toolbox/ptrace transport.
 */
#include <onion/sprx_loader.hpp>
#include <onion/plugin_staging.hpp>
#include "game-runtime/game-lease.h"
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
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

namespace onion::sprx {
namespace {
#ifndef HYPERHEN_PLUGIN_ATTEMPT_ROOT
#define HYPERHEN_PLUGIN_ATTEMPT_ROOT ONION_SYSTEM_TMP_ROOT "/plugin-modules-r50"
#endif
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
std::string receipt_path(const GameIdentity &id, std::string_view name) {
  SHA256_CTX sha; unsigned char digest[32]; char hex[65]{};
  sha256_init(&sha); sha256_update(&sha, reinterpret_cast<const uint8_t *>(name.data()), name.size()); sha256_final(&sha, digest);
  for(unsigned i=0;i<32;++i) snprintf(hex+i*2, 3, "%02x", digest[i]);
  char result[256];
  snprintf(result, sizeof(result), "%s/%d-%lld-%lld-%s.state",
           HYPERHEN_PLUGIN_ATTEMPT_ROOT, id.pid, static_cast<long long>(id.seconds),
           static_cast<long long>(id.microseconds), hex);
  return result;
}
int read_receipt(const GameIdentity &id, std::string_view name, Receipt &r) {
  const std::string path = receipt_path(id, name);
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
  if(mkdir(ONION_SYSTEM_TMP_ROOT, 0777) && errno != EEXIST) return false;
  if(mkdir(HYPERHEN_PLUGIN_ATTEMPT_ROOT, 0700) && errno != EEXIST) return false;
  Receipt stored = r;
  memset(stored.name, 0, sizeof(stored.name)); memcpy(stored.name, name.data(), name.size());
  return write_file_atomic(receipt_path(id, name).c_str(), &stored, sizeof(stored));
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
  if(unlink(receipt_path(id,name).c_str()) && errno!=ENOENT)return false;
  // Remove only the receipt just read. An old alias may refer to content that
  // has since loaded successfully through a different alias. Its shared
  // content receipt must be re-read and reconciled, never deleted via alias.
  return true;
}
#ifndef HYPERHEN_PLUGIN_LEGACY_ROOT
#define HYPERHEN_PLUGIN_LEGACY_ROOT ONION_SYSTEM_TMP_ROOT "/plugin-modules"
#endif
bool previous_runtime_attempt(const GameIdentity &id){
  DIR *dir=opendir(HYPERHEN_PLUGIN_LEGACY_ROOT);
  if(!dir)return errno!=ENOENT;
  char prefix[96];snprintf(prefix,sizeof(prefix),"%d-%lld-%lld-",id.pid,
    static_cast<long long>(id.seconds),static_cast<long long>(id.microseconds));
  bool found=false;unsigned count=0;
  while(dirent *entry=readdir(dir)){
    if(++count>16384 || !strncmp(entry->d_name,prefix,strlen(prefix))){found=true;break;}
  }
  closedir(dir);return found;
}
bool attempt_slot_available(const GameIdentity &id) {
  DIR *dir = opendir(HYPERHEN_PLUGIN_ATTEMPT_ROOT);
  if(!dir) return errno == ENOENT;
  char prefix[96]; snprintf(prefix, sizeof(prefix), "%d-%lld-%lld-", id.pid,
    static_cast<long long>(id.seconds), static_cast<long long>(id.microseconds));
  unsigned total = 0, inspected = 0;
  while(dirent *entry = readdir(dir)) {
    if(++inspected > 16384) { closedir(dir); return false; }
    if(strncmp(entry->d_name, prefix, strlen(prefix))) continue;
    int fd = openat(dirfd(dir), entry->d_name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    Receipt r; struct stat st{};
    bool ok = fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
       st.st_size == sizeof(r) && read(fd, &r, sizeof(r)) == sizeof(r) &&
       r.magic == kReceiptMagic && strnlen(r.name, sizeof(r.name)) < sizeof(r.name);
    if(fd >= 0) close(fd);
    if(!ok) { closedir(dir); return false; }
    if(!strncmp(r.name, "sha256:", 7)) ++total;
  }
  closedir(dir); return total < 8;
}
LoadResult result_for(LoadStatus status, pid_t pid, bool clean = true) {
  LoadResult r; r.status = r.underlying_status = status; r.pid = pid; r.cleanup_ok = clean; return r;
}
class Session {
  pid_t pid_; uintptr_t old_=0; int owned_=0; bool ready_=false;
public:
  explicit Session(pid_t pid):pid_(pid){old_=set_ucred_to_ptrace();ready_=old_ && hhp_plugin_attach_owned(pid_,&owned_)==0;}
  bool ready()const{return ready_;}
  bool detach(){if(!owned_)return true;if(pt_detach(pid_,0))return false;owned_=0;ready_=false;return true;}
  LoadResult finish(LoadResult r){
    if(!detach()){r.underlying_status=r.status;r.status=LoadStatus::AttachFailed;r.cleanup_ok=false;}
    return r;
  }
  ~Session(){detach();if(old_)(void)set_proc_authid(getpid(),old_);}
};
struct GateReceipt {
  uint64_t magic=UINT64_C(0x4848504741544501),control=0,arena=0,cookie=0;
  int64_t boot_seconds=0,boot_microseconds=0,seconds=0,microseconds=0;
  int32_t pid=-1;uint32_t state=Intent;
  intptr_t rootdir=0,jaildir=0;
};
std::string gate_path(const GameIdentity &id){auto p=receipt_path(id,"@game-callback");p.insert(p.find_last_of('/')+1,"gate-");return p;}
bool gate_identity(void *value){return same_game(*static_cast<GameIdentity*>(value));}
int identity_callback(void *value){return gate_identity(value)?1:0;}
bool acquire_gate(const GameIdentity &id,GateReceipt &record,TacGameGate &gate){
  const auto path=gate_path(id);timeval boot{};
  if(!boot_time(boot))return false;
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
  if(mkdir(ONION_SYSTEM_TMP_ROOT,0777)&&errno!=EEXIST)return false;
  if(mkdir(HYPERHEN_PLUGIN_ATTEMPT_ROOT,0700)&&errno!=EEXIST)return false;
  record={};record.pid=id.pid;record.seconds=id.seconds;record.microseconds=id.microseconds;
  record.rootdir=id.rootdir;record.jaildir=id.jaildir;
  record.boot_seconds=boot.tv_sec;record.boot_microseconds=boot.tv_usec;
  do{arc4random_buf(&record.cookie,sizeof(record.cookie));}while(!record.cookie);
  if(!write_file_atomic(path.c_str(),&record,sizeof(record)))return false;
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
  intptr_t scratch = pt_mmap(pid, 0, kPage, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if(scratch <= 0) return 0;
  uint64_t address = 0, size = 0; unsigned char param[32]{}; uint32_t sdk = 0;
  if(!pt_syscall(pid, 598, scratch, scratch+8, 0, 0, 0, 0) &&
     !pt_copyout(pid, scratch, &address, 8) && !pt_copyout(pid, scratch+8, &size, 8) &&
     address >= 0x10000 && address <= UINT64_C(0x7fffffffffff)-32 && size >= 32 && size <= 4096 &&
     !pt_copyout(pid, address, param, sizeof(param))) sdk = hhp_ps5_process_sdk(param, sizeof(param));
  (void)pt_munmap(pid, scratch, kPage); return sdk;
}
}

bool PtraceSprxRuntime::find_loaded(pid_t pid, std::string_view name, ModuleInfo *out) const noexcept {
  if(!out || name.empty() || name.size() >= 256) return false;
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
  GameIdentity id; Receipt receipt;
  if(!capture_game(pid, id)) return result_for(LoadStatus::TargetNotFound, pid);
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
  Session session(pid);
  if(!session.ready()) return session.finish(result_for(LoadStatus::AttachFailed, pid));
  const uint32_t sdk = host_sdk(pid);
  if(!session.detach()) return session.finish(result_for(LoadStatus::AttachFailed, pid, false));
  std::string staged, hash, error;
  if(!stage_module(id, path, sdk, staged, hash, error)) {
    LOG_ERROR("[plugins] stage failed pid=%d: %s", pid, error.c_str());
    return result_for(LoadStatus::AccessPrepareFailed, pid);
  }
  const std::string content_key = "sha256:" + hash;
  int content_prior = read_receipt(id, content_key, receipt);
  if(content_prior==1){(void)refresh_completion(id,content_key,receipt);if(receipt.state==NotDispatched && clear_not_dispatched(id,content_key,receipt))content_prior=0;}
  if(content_prior != 0) {
    if(content_prior == 1) (void)refresh_completion(id,content_key,receipt);
    if(content_prior == 1 && metadata_hash == receipt.metadata_hash && receipt.state == Started && receipt.handle >= 0 &&
       receipt.initializer >= 0 && receipt.activation >= 0 && save_receipt(id, name, receipt)) {
      LoadResult r = result_for(LoadStatus::AlreadyLoaded, pid);
      r.module_handle = static_cast<uint32_t>(receipt.handle); return r;
    }
    if(content_prior==1 && metadata_hash==receipt.metadata_hash && receipt_pending(id,receipt))return result_for(LoadStatus::Pending,pid,true);
    return result_for(LoadStatus::RemoteError, pid, false);
  }
  if(!attempt_slot_available(id)) return result_for(LoadStatus::RemoteError, pid, false);
  GateReceipt gate_record;TacGameGate gate{};
  if(!acquire_gate(id,gate_record,gate))return result_for(LoadStatus::RemoteError,pid,false);
  if(gate.busy)return result_for(LoadStatus::RemoteError,pid,true);
  unsigned slot_index=TAC_GAME_MODULE_SLOTS;
  for(unsigned i=0;i<TAC_GAME_MODULE_SLOTS;i++)if(!gate.modules[i].enabled && gate.modules[i].state==TAC_MODULE_PENDING){slot_index=i;break;}
  // A confirmed failure to create a thread can reclaim its slot while stopped;
  // STARTING/FAILED/STARTED and unknown outcomes are never recycled.
  if(slot_index==TAC_GAME_MODULE_SLOTS)for(unsigned i=0;i<TAC_GAME_MODULE_SLOTS;i++){
    const auto &candidate=gate.modules[i];
    if(candidate.state==TAC_MODULE_REJECTED && candidate.phase==TAC_MODULE_PHASE_THREAD && candidate.threadResult){slot_index=i;break;}
  }
  if(slot_index==TAC_GAME_MODULE_SLOTS)return result_for(LoadStatus::RemoteError,pid,true);
  Session launch(pid);
  if(!launch.ready() || !same_game(id))return launch.finish(result_for(LoadStatus::AttachFailed,pid,true));
  TacGameGate stopped{};
  if(!read_gate(id,gate_record.control,gate_record.cookie,stopped) || stopped.busy ||
     memcmp(&stopped.modules[slot_index],&gate.modules[slot_index],sizeof(TacGameModule)))
    return launch.finish(result_for(LoadStatus::RemoteError,pid,true));
  std::string current_version,current_hash;
  if(!game_version(id,current_version,current_hash) || current_hash!=metadata_hash)
    return launch.finish(result_for(LoadStatus::PolicyDenied,pid,true));
  TacGameModule slot{};slot.handle=-1;
  if(staged.size()>=sizeof(slot.path))return launch.finish(result_for(LoadStatus::InvalidPath,pid,true));
  memcpy(slot.path,staged.c_str(),staged.size()+1);
  const uint64_t address=gate_record.control+offsetof(TacGameGate,modules)+slot_index*sizeof(TacGameModule);
  uint32_t disabled=0;
  if(pt_copyin(pid,&disabled,address+offsetof(TacGameModule,enabled),sizeof(disabled)))
    return launch.finish(result_for(LoadStatus::WriteFailed,pid,false));
  if(pt_copyin(pid,&slot,address,sizeof(slot)))return launch.finish(result_for(LoadStatus::WriteFailed,pid,true));
  timeval boot{};if(!boot_time(boot)||!same_game(id))return launch.finish(result_for(LoadStatus::TargetExited,pid,true));
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
    return launch.finish(result_for(LoadStatus::WriteFailed,pid,true));
  }
  uint32_t enabled=1;
  if(pt_copyin(pid,&enabled,address+offsetof(TacGameModule,enabled),sizeof(enabled)))
    return launch.finish(result_for(LoadStatus::WriteFailed,pid,false));
  if(!launch.detach())return launch.finish(result_for(LoadStatus::AttachFailed,pid,false));
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
    if(receipt_pending(id,receipt))return result_for(LoadStatus::Pending,pid,true);
    return result_for(same_game(id)?LoadStatus::Timeout:LoadStatus::TargetExited,pid,false);
  }
  const bool success=receipt.state==Started && receipt.handle>=0 && receipt.initializer>=0 && receipt.activation>=0;
  LOG_INFO("[plugins] initialized pid=%d module=%.*s handle=%08x start=%08x activation=%08x state=%u",pid,
    static_cast<int>(name.size()),name.data(),static_cast<unsigned>(receipt.handle),static_cast<unsigned>(receipt.initializer),
    static_cast<unsigned>(receipt.activation),receipt.state);
  LoadResult result=result_for(success?LoadStatus::Loaded:receipt.state==NotDispatched?LoadStatus::ThreadCreateFailed:LoadStatus::RemoteError,pid,success||receipt.state==NotDispatched);
  if(success)result.module_handle=static_cast<uint32_t>(receipt.handle);
  result.remote_result=receipt.handle<0?receipt.handle:receipt.initializer;
  return result;
}
}
