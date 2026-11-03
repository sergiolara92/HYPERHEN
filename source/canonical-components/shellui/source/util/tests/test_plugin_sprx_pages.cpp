#include "test_harness.h"

#include "plugin_sprx_pages.hpp"
#include "toolbox_i18n.hpp"
#include "toolbox_route.hpp"

#include <string>

using namespace onion::shellui::plugin_pages;

static int test_plugin_list_is_flat_links(void) {
  toolbox_i18n::set_lang(toolbox_i18n::Lang::En);
  std::vector<PluginInventoryItem> plugins = {
      {.plugin_id = "FTPS00001",
       .version = "1.00",
       .name = "FTP Server",
       .running = true,
       .auto_start = true},
      {.plugin_id = "HYPD00001",
       .version = "1.00",
       .name = "HyperDPI",
       .running = false,
       .auto_start = false},
  };

  ps5ui::Page page("id_plugins", "Plugins");
  append_plugin_list_links(page, plugins);
  const std::string xml = page.build();

  TEST_ASSERT_TRUE(xml.find("id=\"id_external_plugins\"") == std::string::npos);
  TEST_ASSERT_TRUE(xml.find("setting_list id=\"id_external_plugin_") ==
                   std::string::npos);
  TEST_ASSERT_TRUE(
      xml.find("file=\"plugin_FTPS00001.xml\"") != std::string::npos);
  TEST_ASSERT_TRUE(xml.find("file=\"plugin_HYPD00001.xml\"") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(xml.find("second_title=\"Running · Auto-start\"") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(xml.find("second_title=\"Stopped\"") != std::string::npos);
  TEST_ASSERT_TRUE(xml.find("id_external_plugin_run_") == std::string::npos);
  return 0;
}

static int test_plugin_config_order(void) {
  toolbox_i18n::set_lang(toolbox_i18n::Lang::En);
  const PluginInventoryItem plugin{.plugin_id = "FTPS00001",
                                   .version = "1.00",
                                   .name = "FTP Server",
                                   .running = true,
                                   .auto_start = false};
  onion::shellui::dynamic_ui::PluginSettingsLink settings{
      .plugin_id = "FTPS00001",
      .control_id = "id_settings",
      .title = "FTP Settings",
      .resource = "onion_ui_ftp.xml",
      .description = "Ports and auth",
  };

  ps5ui::Page page("id_plugin_config", plugin.name);
  fill_plugin_config(page, plugin, &settings);
  const std::string xml = page.build();

  const auto run_at = xml.find("id=\"id_external_plugin_run_FTPS00001\"");
  const auto auto_at =
      xml.find("id=\"id_external_plugin_autostart_FTPS00001\"");
  const auto settings_at =
      xml.find("id=\"id_external_plugin_settings_FTPS00001\"");
  const auto delete_at = xml.find("id=\"id_external_plugin_delete_FTPS00001\"");
  TEST_ASSERT_TRUE(run_at != std::string::npos);
  TEST_ASSERT_TRUE(auto_at != std::string::npos);
  TEST_ASSERT_TRUE(settings_at != std::string::npos);
  TEST_ASSERT_TRUE(delete_at != std::string::npos);
  TEST_ASSERT_TRUE(run_at < auto_at);
  TEST_ASSERT_TRUE(auto_at < settings_at);
  TEST_ASSERT_TRUE(settings_at < delete_at);
  TEST_ASSERT_TRUE(xml.find("file=\"onion_ui_ftp.xml\"") != std::string::npos);
  return 0;
}

static int test_standalone_manual_only(void) {
  toolbox_i18n::set_lang(toolbox_i18n::Lang::En);
  PluginInventoryItem plugin;
  plugin.plugin_id = "TEST00001"; plugin.name = "Standalone"; plugin.manual_only = true;
  ps5ui::Page page("id_plugin_config", plugin.name);
  fill_plugin_config(page, plugin, nullptr);
  const auto xml = page.build();
  TEST_ASSERT_TRUE(xml.find("id_external_plugin_run_TEST00001") != std::string::npos);
  TEST_ASSERT_TRUE(xml.find("id_external_plugin_autostart_") == std::string::npos);
  TEST_ASSERT_TRUE(xml.find("Reboot to unload") != std::string::npos);
  return 0;
}

static int test_sprx_list_is_flat_links(void) {
  toolbox_i18n::set_lang(toolbox_i18n::Lang::En);
  std::vector<SprxInventoryItem> sprx = {
      {.id = "overlay",
       .path = "/data/HyperHEN/sprx/overlay.sprx",
       .enabled = true,
       .loaded_for_current_target = true},
      {.id = "fps.overlay",
       .path = "/data/HyperHEN/sprx/fps.sprx",
       .enabled = true,
       .matches_current_target = true},
  };

  ps5ui::Page page("id_sprx", "SPRX");
  append_sprx_list_links(page, sprx);
  const std::string xml = page.build();

  TEST_ASSERT_TRUE(xml.find("id=\"id_external_sprx\"") == std::string::npos);
  TEST_ASSERT_TRUE(xml.find("setting_list id=\"id_external_sprx_") ==
                   std::string::npos);
  TEST_ASSERT_TRUE(xml.find("file=\"sprx_overlay.xml\"") != std::string::npos);
  TEST_ASSERT_TRUE(xml.find("file=\"sprx_fps.overlay.xml\"") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(xml.find("second_title=\"Loaded in the current game\"") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(
      xml.find(
          "second_title=\"Matching game: manual loading available\"") !=
      std::string::npos);
  TEST_ASSERT_TRUE(xml.find("id_external_sprx_enabled_") == std::string::npos);
  return 0;
}

static int test_sprx_config_has_status_and_path(void) {
  toolbox_i18n::set_lang(toolbox_i18n::Lang::En);
  const SprxInventoryItem entry{.id = "overlay",
                                .path = "/data/HyperHEN/sprx/overlay.sprx",
                                .enabled = false,
                                .matches_current_target = true};

  ps5ui::Page page("id_sprx_config", entry.id);
  fill_sprx_config(page, entry);
  const std::string xml = page.build();

  TEST_ASSERT_TRUE(xml.find("id=\"id_external_sprx_status_overlay\"") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(xml.find("/data/HyperHEN/sprx/overlay.sprx") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(xml.find("id=\"id_external_sprx_enabled_overlay\"") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(xml.find("value=\"0\"") != std::string::npos);
  TEST_ASSERT_TRUE(xml.find("id=\"id_external_sprx_delete_overlay\"") !=
                   std::string::npos);
  return 0;
}

static int test_discovered_sprx_name_and_manual_registration(void) {
  toolbox_i18n::set_lang(toolbox_i18n::Lang::En);
  SprxInventoryItem candidate{.id="file.123456789012345678901234",
      .path="/data/plugins/RaiDougie<&.prx",.registered=false};
  TEST_ASSERT_TRUE(sprx_display_name(candidate)=="RaiDougie<&.prx");
  TEST_ASSERT_TRUE(sprx_list_status(candidate)=="Not registered for the current game");
  ps5ui::Page list("id_sprx","SPRX");
  append_sprx_list_links(list,{candidate});
  const auto xml=list.build();
  TEST_ASSERT_TRUE(xml.find("RaiDougie&lt;&amp;.prx")!=std::string::npos);
  TEST_ASSERT_TRUE(xml.find("sprx_file.123456789012345678901234.xml")!=std::string::npos);
  ps5ui::Page config("id_sprx_config",sprx_display_name(candidate));
  fill_sprx_config(config,candidate);
  const auto config_xml=config.build();
  TEST_ASSERT_TRUE(config_xml.find("Register and load in current game")!=std::string::npos);
  TEST_ASSERT_TRUE(config_xml.find("id_external_sprx_enabled_file.")!=std::string::npos);
  TEST_ASSERT_TRUE(config_xml.find("id_external_sprx_delete_")==std::string::npos);
  TEST_ASSERT_TRUE(config_xml.find("value=\"0\"")!=std::string::npos);
  candidate.registered=true;
  TEST_ASSERT_TRUE(sprx_display_name(candidate)=="RaiDougie<&.prx");
  ps5ui::Page registered("id_sprx_config",sprx_display_name(candidate));
  fill_sprx_config(registered,candidate);
  TEST_ASSERT_TRUE(registered.build().find("id_external_sprx_delete_")!=std::string::npos);
  return 0;
}

static int test_discovered_names_are_bounded_and_safe(void) {
  SprxInventoryItem entry{.id="file.123456789012345678901234",.path="/data/plugins/\xe2\x80\xaehidden\n<&.prx",.registered=false};
  TEST_ASSERT_TRUE(sprx_display_name(entry)=="hidden <&.prx");
  entry.path="/data/plugins/"+std::string(1000,'a');
  TEST_ASSERT_TRUE(sprx_display_name(entry).size()==240);
  entry.path="/data/plugins/";
  TEST_ASSERT_TRUE(sprx_display_name(entry)==entry.id);
  SprxInventoryItem old{.id="legacy",.path="/data/legacy.prx"};
  TEST_ASSERT_TRUE(old.registered);
  TEST_ASSERT_TRUE(sprx_display_name(old)=="legacy");
  return 0;
}

static int test_plugins_page_exposes_sprx_link(void) {
  toolbox_i18n::set_lang(toolbox_i18n::Lang::En);
  ps5ui::Page page("id_plugins","Plugins");
  append_sprx_page_link(page);
  const auto xml=page.build();
  TEST_ASSERT_TRUE(xml.find("id_plugins_sprx")!=std::string::npos);
  TEST_ASSERT_TRUE(xml.find("file=\"sprx.xml\"")!=std::string::npos);
  return 0;
}

extern "C" int test_plugin_sprx_pages_suite(void) {
  int fails = 0;
  fails += onion_test_run("plugin_pages.flat_list", test_plugin_list_is_flat_links);
  fails += onion_test_run("plugin_pages.config_order", test_plugin_config_order);
  fails += onion_test_run("plugin_pages.manual_only", test_standalone_manual_only);
  fails += onion_test_run("sprx_pages.flat_list", test_sprx_list_is_flat_links);
  fails += onion_test_run("sprx_pages.config_status",
                          test_sprx_config_has_status_and_path);
  fails += onion_test_run("sprx_pages.discovered_manual",test_discovered_sprx_name_and_manual_registration);
  fails += onion_test_run("sprx_pages.bounded_names",test_discovered_names_are_bounded_and_safe);
  fails += onion_test_run("plugin_pages.sprx_link",test_plugins_page_exposes_sprx_link);
  return fails;
}
