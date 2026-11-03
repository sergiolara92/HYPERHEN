#include "plugin_sprx_pages.hpp"

#include "toolbox_i18n.hpp"
#include "toolbox_route.hpp"
#include <onion/plugin_diagnostic.hpp>

namespace onion::shellui::plugin_pages {

std::string plugin_list_status(bool running, bool auto_start) {
  if (running && auto_start)
    return toolbox_i18n::tr("plugins.external.status.running_autostart");
  if (running)
    return toolbox_i18n::tr("plugins.external.status.running");
  if (auto_start)
    return toolbox_i18n::tr("plugins.external.status.stopped_autostart");
  return toolbox_i18n::tr("plugins.external.status.stopped");
}

std::string sprx_list_status(const SprxInventoryItem &entry) {
  if (!entry.registered)
    return toolbox_i18n::tr("sprx.discovery.unregistered");
  if (entry.loaded_for_current_target)
    return toolbox_i18n::tr("sprx.status.loaded");
  if (entry.matches_current_target)
    return toolbox_i18n::tr("sprx.manual.matches");
  return toolbox_i18n::tr("sprx.status.inactive");
}

std::string sprx_display_name(const SprxInventoryItem &entry) {
  if (!entry.registered || entry.id.starts_with("file.")) {
    const std::string_view path(entry.path);
    const auto slash = path.find_last_of("/\\");
    const auto leaf = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::string name = onion::ipc::plugin_diagnostic(leaf);
    if (!name.empty()) return name;
  }
  return onion::ipc::plugin_diagnostic(entry.id);
}

void append_sprx_page_link(ps5ui::Page &page) {
  page.link("id_plugins_sprx", toolbox_i18n::tr("sprx.link"),
            std::string(toolbox::kSprxXml.substr(toolbox::kSettingsPluginsPrefix.size())),
            toolbox_i18n::tr("sprx.link.sub"));
}

void append_plugin_list_links(
    ps5ui::Page &page, const std::vector<PluginInventoryItem> &plugins) {
  for (const PluginInventoryItem &plugin : plugins)
    page.link(std::string(kPluginItemPrefix) + plugin.plugin_id, plugin.name,
              toolbox::external_plugin_config_xml(plugin.plugin_id),
              plugin_list_status(plugin.running, plugin.auto_start));
}

void fill_plugin_config(ps5ui::Page &page, const PluginInventoryItem &plugin,
                        const dynamic_ui::PluginSettingsLink *settings) {
  page.toggle(std::string(kPluginRunPrefix) + plugin.plugin_id,
              toolbox_i18n::tr("plugins.external.run"), plugin.running,
              toolbox_i18n::tr(plugin.manual_only ? "plugins.external.manual_only" : "plugins.external.run.sub"));
  if (!plugin.manual_only)
    page.toggle(std::string(kPluginAutoStartPrefix) + plugin.plugin_id,
              toolbox_i18n::tr("plugins.external.autostart"), plugin.auto_start,
              toolbox_i18n::tr("plugins.external.autostart.sub"));
  if (settings)
    page.link("id_external_plugin_settings_" + plugin.plugin_id, settings->title,
              settings->resource,
              settings->description.empty()
                  ? std::optional<std::string>{}
                  : std::optional<std::string>{settings->description});
  page.button(std::string(kPluginDeletePrefix) + plugin.plugin_id,
              toolbox_i18n::tr("plugins.external.delete"), std::nullopt,
              std::nullopt, std::nullopt, ps5ui::Style::None,
              toolbox_i18n::tr("plugins.external.delete.confirm"),
              toolbox_i18n::tr("account.activate.confirm_phrase"));
}

void append_sprx_list_links(ps5ui::Page &page,
                            const std::vector<SprxInventoryItem> &sprx) {
  for (const SprxInventoryItem &entry : sprx)
    page.link(std::string(kSprxItemPrefix) + entry.id, sprx_display_name(entry),
              toolbox::sprx_config_xml(entry.id), sprx_list_status(entry));
}

void fill_sprx_config(ps5ui::Page &page, const SprxInventoryItem &entry) {
  page.label("id_external_sprx_status_" + entry.id, sprx_list_status(entry))
      .label("id_external_sprx_path_" + entry.id, entry.path)
      .toggle(std::string(kSprxEnabledPrefix) + entry.id,
              toolbox_i18n::tr(entry.registered ? "sprx.manual.label" : "sprx.discovery.load"),
              entry.registered && entry.loaded_for_current_target,
              toolbox_i18n::tr(entry.registered ? "sprx.manual.sub" : "sprx.discovery.load.sub"));
  if (entry.registered)
    page.button(std::string(kSprxDeletePrefix) + entry.id,
              toolbox_i18n::tr("sprx.delete"), std::nullopt,
              toolbox_i18n::tr("sprx.delete.sub"), std::nullopt,
              ps5ui::Style::None, toolbox_i18n::tr("sprx.delete.confirm"));
}

} // namespace onion::shellui::plugin_pages
