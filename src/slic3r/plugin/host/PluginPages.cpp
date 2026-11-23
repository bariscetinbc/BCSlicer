#include "PluginPages.hpp"

#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/Notebook.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/plugin/PluginManager.hpp"

#include <algorithm>

#include <atomic>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <memory>
#include <exception>
#include <cstddef>
#include <nlohmann/json.hpp>

#include "slic3r/plugin/pluginTypes/pages/PagesPluginCapability.hpp"
#include "slic3r/GUI/WebPanel.hpp"
#include <optional>
#include "slic3r/plugin/PythonPluginInterface.hpp"
#include "slic3r/GUI/LazyPage.hpp"
#include <stdexcept>
#include <string>
#include <wx/app.h>
#include <vector>
#include <wx/bookctrl.h>
#include <wx/event.h>
#include <wx/menu.h>
#include <wx/sizer.h>

#include <utility>
#include <wx/webview.h>
#include <wx/string.h>

namespace Slic3r {
namespace {

constexpr char PLUGIN_PAGE_BRIDGE_JS[] = R"JS(
(function () {
  if (window.top !== window.self) return;
  if (window.orca) return;
  var handlers = [];
  function deliver(payload, attempts) {
    try {
      if (window.wx && typeof window.wx.postMessage === 'function') {
        window.wx.postMessage(payload);
        return;
      }
    } catch (e) { /* retry while the native handler is being registered */ }
    if (attempts < 100)
      window.setTimeout(function () { deliver(payload, attempts + 1); }, 25);
  }
  function send(data) {
    deliver(JSON.stringify({
      channel: 'orca', kind: 'message', data: (data === undefined ? null : data)
    }), 0);
  }
  window.orca = {
    postMessage: function (data) { send(data); },
    onMessage: function (callback) {
      if (typeof callback === 'function') handlers.push(callback);
    }
  };
  window.__orcaDispatch = function (payload) {
    var data = payload ? payload.data : null;
    for (var i = 0; i < handlers.length; i++) {
      try { handlers[i](data); } catch (e) {}
    }
  };
})();
)JS";

} // namespace

PluginPage::PluginPage(wxWindow* parent, std::shared_ptr<PagesPluginCapability> capability)
    : GUI::WebPanel(parent, PLUGIN_PAGE_BRIDGE_JS)
    , m_cap(std::move(capability))
    , m_lifetime(std::make_shared<std::atomic<PluginPage*>>(this))
{
    browser()->Bind(wxEVT_WEBVIEW_NEWWINDOW, &PluginPage::on_new_window, this);

    const std::shared_ptr<std::atomic<PluginPage*>> lifetime = m_lifetime;
    m_cap->set_message_sender([lifetime](const std::string& message) {
        if (wxTheApp == nullptr)
            return;

        GUI::wxGetApp().CallAfter([lifetime, message] {
            if (PluginPage* page = lifetime->load(std::memory_order_acquire))
                page->push_message(message);
        });
    });

}

PluginPage::~PluginPage()
{
    detach_capability();
    if (m_lifetime)
        m_lifetime->store(nullptr, std::memory_order_release);
}

void PluginPage::detach_capability()
{
    if (m_lifetime)
        m_lifetime->store(nullptr, std::memory_order_release);
    if (m_cap)
        m_cap->clear_message_sender();
    m_cap.reset();
}

std::optional<std::string> PluginPage::page_html()
{
    if (m_cap == nullptr)
        return std::nullopt;

    try {
        return m_cap->get_ui();
    } catch (const std::exception& error) {
        BOOST_LOG_TRIVIAL(error) << "Failed to load plugin page '" << m_cap->name() << "': " << error.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "Failed to load plugin page '" << m_cap->name() << "'";
    }
    detach_capability();
    return std::nullopt;
}

void PluginPage::on_new_window(wxWebViewEvent& event)
{
    const wxString url = event.GetURL();
    if (!url.empty())
        browser()->LoadURL(url);
    event.Veto();
}

bool PluginPage::on_page_message(const std::string& kind, const nlohmann::json& data)
{
    if (kind != "message")
        return false;
    if (!m_cap)
        return true;

    try {
        m_cap->on_message(data.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    } catch (const std::exception& error) {
        BOOST_LOG_TRIVIAL(error) << "Plugin page message handler failed for '" << m_cap->name() << "': " << error.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "Plugin page message handler failed for '" << m_cap->name() << "'";
    }
    return true;
}

void PluginPage::push_message(const std::string& message)
{
    // PagesPluginCapability::post_message() already dumps JSON, so accept it as-is; only a
    // non-JSON payload needs wrapping as a string literal.
    post_to_page(nlohmann::json::accept(message)
                     ? message
                     : nlohmann::json(message).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}

PluginPages::~PluginPages()
{
    shutdown();
}

void PluginPages::initialize(Notebook* parent)
{
    shutdown();
    m_parent = parent;
    if (m_parent == nullptr)
        return;

    m_visible_page_count = GUI::wxGetApp().app_config->get_plugin_pages_visible_count();

    for (const auto& capability : PluginManager::instance().get_plugin_capabilities("", PluginCapabilityType::Pages)) {
        if (capability)
            create_page(capability->identity());
    }
    relayout();
}

void PluginPages::shutdown()
{
    // Removing the selected tab selects the tab to its left. In tab order that is a built-in tab,
    // never an unbuilt plugin page that is removed next and would be built only to be destroyed.
    for (const PluginCapabilityId& id : std::vector<PluginCapabilityId>(m_order))
        remove_page(id);
    m_parent = nullptr;
}

void PluginPages::set_visible_page_count(int count)
{
    const int clamped = std::clamp(count, PLUGIN_PAGES_VISIBLE_COUNT_MIN, PLUGIN_PAGES_VISIBLE_COUNT_MAX);
    if (clamped == m_visible_page_count)
        return;

    m_visible_page_count = clamped;
    relayout();
}

std::shared_ptr<PagesPluginCapability> PluginPages::get_pages_cap(const PluginCapabilityId& id, bool is_enabled) const
{
    auto capability = PluginManager::instance().get_plugin_capability(id, /*only_enabled=*/false);
    if (!capability || capability->is_enabled() != is_enabled || capability->type() != PluginCapabilityType::Pages)
        return nullptr;

    return std::dynamic_pointer_cast<PagesPluginCapability>(capability);
}

bool PluginPages::create_page(const PluginCapabilityId& id)
{
    if (m_pages.find(id) != m_pages.end())
        return false;

    auto capability = get_pages_cap(id, true);
    if (!capability)
        return false;

    std::string icon;
    try {
        icon = capability->get_icon();
    } catch (const std::exception& error) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " Failed to get icon for plugin " << id.plugin_key << ": " << error.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " Failed to get icon for plugin " << id.plugin_key;
    }

    wxBitmap bitmap;
    if (!icon.empty()) {
        try {
            boost::filesystem::path icon_path(icon);
            const std::string extension = icon_path.extension().string();
            if (extension == ".svg" || extension == ".png")
                icon_path.replace_extension();

            bitmap = create_scaled_bitmap(icon_path.string(), m_parent, 20);
        } catch (const std::exception& error) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " Failed to load icon for plugin " << id.plugin_key << ": " << error.what();
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " Failed to load icon for plugin " << id.plugin_key;
        }
    }

    // Built the first time its tab is shown, so a language switch never creates the page's browser
    // while the main window is being rebuilt. Never prebuilt: the idle queue cannot drop a page
    // that remove_page() destroys.
    auto* page = new GUI::LazyPage<PluginPage>(m_parent, GUI::into_u8(page_tab_id(id)), -1,
                                               [capability = std::move(capability)](wxWindow* parent) {
                                                   return new PluginPage(parent, capability);
                                               });

    m_pages.emplace(id, Page{page, bitmap});
    m_order.push_back(id);
    return true;
}

void PluginPages::on_cap_register(const PluginCapabilityId& id)
{
    if (m_parent == nullptr)
        return;

    if (create_page(id))
        relayout();
}

void PluginPages::on_cap_deregister(const PluginCapabilityId& id)
{
    remove_page(id);
}

void PluginPages::on_plugin_register(const std::string& plugin_key)
{
    for (const auto& capability : PluginManager::instance().get_plugin_capabilities(plugin_key, PluginCapabilityType::Pages)) {
        if (capability)
            on_cap_register(capability->identity());
    }
}

void PluginPages::on_plugin_deregister(const std::string& plugin_key)
{
    // In tab order, as in shutdown().
    for (const PluginCapabilityId& id : std::vector<PluginCapabilityId>(m_order))
        if (id.plugin_key == plugin_key)
            remove_page(id);
}

void PluginPages::remove_page(const PluginCapabilityId& id)
{
    auto it = m_pages.find(id);
    if (it == m_pages.end())
        return;

    GUI::LazyPage<PluginPage>* page = it->second.page;
    // Only a built page has installed a message sender on the capability.
    if (PluginPage* built = page->get())
        built->detach_capability();

    m_pages.erase(it);
    m_order.erase(std::remove(m_order.begin(), m_order.end(), id), m_order.end());

    const int idx = m_parent != nullptr ? m_parent->FindPage(page) : wxNOT_FOUND;
    if (idx != wxNOT_FOUND)
        m_parent->RemovePage(idx);

    relayout();
    page->Destroy();
}

wxString PluginPages::page_tab_id(const PluginCapabilityId& id)
{
    return wxString::FromUTF8("plugin." + id.plugin_key + "." + id.name);
}

// Flash Studio's tab bar (Orca 2.3 based) has no named tabs and no overflow button, so every
// plugin page gets its own tab after the built-in ones, tracked by its window.
void PluginPages::relayout()
{
    if (m_parent == nullptr)
        return;

    m_order.erase(std::remove_if(m_order.begin(), m_order.end(),
        [this](const PluginCapabilityId& id) {
            const bool orphaned = m_pages.find(id) == m_pages.end();
            if (orphaned)
                BOOST_LOG_TRIVIAL(error) << "PluginPages::relayout: '" << id.name << "' was in m_order but not m_pages, dropping";
            return orphaned;
        }),
        m_order.end());
    m_swapped_in_id.reset();

    // Only touch the notebook when the trailing tabs aren't already the plugin pages in order.
    const size_t page_count = m_parent->GetPageCount();
    bool up_to_date = page_count >= m_order.size();
    for (size_t i = 0; up_to_date && i < m_order.size(); ++i)
        up_to_date = m_parent->GetPage(page_count - m_order.size() + i) == m_pages.at(m_order[i]).page;
    for (const auto& [id, entry] : m_pages) {
        if (!up_to_date)
            break;
        up_to_date = entry.page->in_book();
    }
    if (up_to_date)
        return;

    wxWindow* to_reselect = m_parent->GetCurrentPage();
    for (const auto& id : m_order) {
        const int idx = m_parent->FindPage(m_pages.at(id).page);
        if (idx != wxNOT_FOUND) {
            m_parent->RemovePage(idx);
            m_pages.at(id).page->Hide();
        }
    }
    for (const auto& id : m_order)
        m_parent->InsertPage(m_parent->GetPageCount(), m_pages.at(id).page, wxString::FromUTF8(id.name), std::string(), std::string(), false);

    if (to_reselect != nullptr) {
        const int idx = m_parent->FindPage(to_reselect);
        if (idx != wxNOT_FOUND)
            m_parent->SetSelection(idx);
    }
}

void PluginPages::select_page(const PluginCapabilityId& id)
{
    if (m_parent == nullptr || m_pages.find(id) == m_pages.end())
        return;
    relayout();
    const int idx = m_parent->FindPage(m_pages.at(id).page);
    if (idx != wxNOT_FOUND)
        m_parent->SetSelection(idx);
}

void PluginPages::show_overflow_menu()
{
    // Flash Studio shows every plugin page as its own tab.
}

} // namespace Slic3r
