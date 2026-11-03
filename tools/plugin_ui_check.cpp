#include <onion/sprx_catalog.hpp>
#include <onion/ipc_client.hpp>
#include <onion/ipc_server.hpp>
#include <onion/plugin_diagnostic.hpp>
#include <onion/notify_i18n.h>
#include "ps5_settings_ui.hpp"
#include "toolbox_i18n.hpp"
#include "onion_cjson.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sys/socket.h>
#include <unistd.h>

namespace fs = std::filesystem;
static int peer = -1;
static bool reply_error = true;
static std::string reply_text;
static unsigned exchanges;
namespace onion {
int ipc_unix_connect(const char *) {
  int pair[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  if (peer >= 0) close(peer);
  peer = pair[1]; return pair[0];
}
int ipc_network_send_full(int, const void *data, int32_t size) {
  assert(size == sizeof(IPCMessage));
  const auto &req = *static_cast<const IPCMessage *>(data);
  assert(req.magic == 0xDEADBABE);
  onion_cjson::Root body(req.msg); assert(body);
  if (req.cmd != BREW_SPRX_LIST)
    assert(onion_cjson::string_item(body.get(), req.cmd == BREW_PLUGIN_START ||
       req.cmd == BREW_PLUGIN_STOP ? "plugin_id" : "id"));
  ++exchanges;
  IPCMessage reply{};reply.magic=0xDEADBABE;reply.cmd=BREW_RETURN_VALUE;
  reply.error=reply_error?-1:0;
  auto *j=cJSON_CreateObject();cJSON_AddNumberToObject(j,"res",reply.error);
  cJSON_AddStringToObject(j,"var",reply_text.c_str());
  auto text=onion_cjson::print_owned(j);
  assert(text.size()<sizeof(reply.msg));memcpy(reply.msg,text.c_str(),text.size()+1);
  assert(send(peer,&reply,sizeof(reply),0)==sizeof(reply));
  return size;
}
int ipc_network_recv_full(int fd,void *data,int32_t size) {
  size_t total=0;
  while(total<static_cast<size_t>(size)) {
    auto n=recv(fd,static_cast<char*>(data)+total,size-total,0);
    if(n<=0)return n;total+=n;
  }
  return size;
}
}
extern "C" const char *onion_notify_tr(const char *key) { return key; }
static onion_notify_language_t ui_language=ONION_NOTIFY_LANG_EN;
extern "C" onion_notify_language_t onion_notify_get_ui_language() {return ui_language;}
extern "C" void onion_notify_set_ui_language(onion_notify_language_t value) {ui_language=value;}

static std::string read(const fs::path &p) {
  std::ifstream f(p);return {std::istreambuf_iterator<char>(f),{}};
}
static void write(const fs::path &p,const std::string &s) {
  std::ofstream f(p);f<<s;assert(f.good());
}
int main() {
  using onion::ipc::plugin_diagnostic;
  assert(plugin_diagnostic("failure\nline\t2\x1b") == "failure line 2");
  assert(plugin_diagnostic("bad\xe2\x80\xae name") == "bad name");
  assert(plugin_diagnostic(std::string(239,'a')+"\xc3\xa8").size()==239);
  assert(plugin_diagnostic(std::string(1000,'a')).size()==240);
  assert(plugin_diagnostic("\xc0\xaf")=="??");
  assert(plugin_diagnostic("\xed\xa0\x80")=="???");
  assert(plugin_diagnostic("\xf4\x90\x80\x80")=="????");
  assert(plugin_diagnostic("Errore gi\xc3\xa0 rilevato")=="Errore gi\xc3\xa0 rilevato");
  ps5ui::Page page("diagnostic","Plugins");
  page.label("error",plugin_diagnostic("<script>&\"not markup\""));
  const auto xml=page.build();
  assert(xml.find("<script>")==std::string::npos);
  assert(xml.find("&lt;script&gt;&amp;")!=std::string::npos);
  for(int lang=0;lang<28;++lang) {
    toolbox_i18n::set_lang(static_cast<toolbox_i18n::Lang>(lang));
    const auto text=std::string(toolbox_i18n::tr("sprx.manual.pending"));
    assert(!text.empty()&&text!="sprx.manual.pending");
    assert(plugin_diagnostic(text)==text);
  }
  toolbox_i18n::set_lang(toolbox_i18n::Lang::It);
  assert(std::string(toolbox_i18n::tr("sprx.manual.pending"))==
         "In attesa: torna al gioco per completare il caricamento del plugin.");

  auto &ipc=IPC_Client::getInstance(false);std::string detail="old";
  reply_text="Catalog line 2: invalid <path>\n\x1b";
  assert(!ipc.StartPlugin("TEST00001",&detail));
  assert(detail=="Catalog line 2: invalid <path>");
  assert(!ipc.StopPlugin("TEST00001",&detail));assert(!detail.empty());
  assert(!ipc.SetSprxEnabled("overlay",true,&detail));assert(!detail.empty());
  assert(!ipc.DeleteSprx("overlay",&detail));assert(!detail.empty());
  std::vector<SprxInventoryItem> items{{.id="stale"}};
  assert(!ipc.ListSprx(items,&detail));assert(items.empty());
  assert(detail=="Catalog line 2: invalid <path>");
  reply_text=std::string(1500,'a');
  assert(!ipc.SetSprxEnabled("overlay",true,&detail));assert(detail.size()==240);
  reply_error=false;reply_text="ok";
  assert(ipc.StartPlugin("TEST00001",&detail));assert(detail.empty());
  reply_text="pending-return-to-game";
  assert(ipc.SetSprxEnabled("overlay",true,&detail));
  assert(onion::ipc::plugin_operation_pending(true,detail));
  assert(!onion::ipc::plugin_operation_pending(false,detail));
  assert(!onion::ipc::plugin_operation_pending(true,"ok"));
  reply_error=true;
  assert(!ipc.SetSprxEnabled("overlay",true,&detail));
  assert(!onion::ipc::plugin_operation_pending(false,detail));
  reply_error=false;
  reply_text="{\"sprx\":[],\"next\":-1}";
  assert(ipc.ListSprx(items,&detail));assert(items.empty()&&detail.empty());
  ipc.IPCCloseConnection();close(peer);peer=-1;

  char temp[]="/tmp/hyperhen-plugin-ui-XXXXXX";assert(mkdtemp(temp));
  const fs::path dir=temp,path=dir/"catalog.ini",stale=dir/"catalog.ini.writing";
  const std::string manifest="[plugin.overlay]\npath=/data/overlay.prx\nexact_title_ids=PPSA00001\n";
  write(path,manifest);write(stale,"another interrupted writer");
  onion::sprx::SprxCatalogStore store;
  assert(store.load(path.string()));
  assert(store.bind_game("overlay","PPSA00001","01.010.000"));
  assert(read(stale)=="another interrupted writer");
  assert(read(path).find("game_version=01.010.000")!=std::string::npos);
  size_t files=0;for(auto &entry:fs::directory_iterator(dir)){(void)entry;++files;}assert(files==2);
  const auto original=read(path);
  fs::rename(path,dir/"preserved.ini");fs::create_directory(path);
  assert(!store.set_enabled("overlay",false));
  assert(store.snapshot().find("overlay")->enabled);
  assert(read(dir/"preserved.ini")==original);
  files=0;for(auto &entry:fs::directory_iterator(dir)){(void)entry;++files;}assert(files==3);
  fs::remove(path);fs::rename(dir/"preserved.ini",path);
  assert(store.set_enabled("overlay",false));
  assert(store.remove("overlay"));assert(read(path).empty());
  fs::remove_all(dir);
  std::printf("PASS catalog stale temporary, failed-rename rollback/cleanup, diagnostic UTF8/length/XML escaping, actual IPC response path and distinct pending status,28locale runtime lookups; exchanges=%u\n",exchanges);
}
