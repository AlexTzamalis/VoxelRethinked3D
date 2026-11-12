#include "resource_packs_screen.hpp"

#include <vxe/core/log.hpp>
#include <vxe/platform/platform.hpp>
#include <vxe/ui/controls.hpp>

#include <algorithm>
#include <format>
#include <utility>
#include <variant>

namespace vr {
namespace {

using vxe::f32;
using vxe::usize;

constexpr f32 kPanelWidth = 560.0F;
constexpr std::string_view kPacksSetting = "Graphics.Textures.Packs";
constexpr std::string_view kDefaultPack = "default";

std::string trimmed(std::string_view text) {
    const usize first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    const usize last = text.find_last_not_of(" \t");
    return std::string(text.substr(first, last - first + 1));
}

vxe::ui::Widget& row(vxe::ui::Widget& parent) {
    auto& widget = parent.add<vxe::ui::Widget>();
    widget.style().direction = vxe::ui::Direction::Row;
    widget.style().gap = 4.0F;
    return widget;
}

vxe::ui::Button& wideButton(vxe::ui::Widget& parent, std::string text, std::function<void()> onClick) {
    auto& button = parent.add<vxe::ui::Button>(std::move(text), std::move(onClick));
    button.style().grow = 1.0F;
    button.style().minWidth = 0.0F;
    return button;
}

// One list with its heading, filling half of the panel.
vxe::ui::Widget& column(vxe::ui::Widget& parent, std::string title) {
    using namespace vxe::ui;
    auto& side = parent.add<Widget>();
    side.style().grow = 1.0F;
    side.style().width = 0.0F; // With grow: both lists get half, whatever their names.
    side.style().gap = 2.0F;
    side.add<Label>(std::move(title), TextRole::Muted).setAlign(TextAlign::Center);
    auto& scroll = side.add<ScrollView>();
    scroll.style().grow = 1.0F;
    auto& list = scroll.add<Widget>();
    list.style().gap = 2.0F;
    return list;
}

} // namespace

std::vector<std::string> parsePackList(std::string_view text) {
    std::vector<std::string> packs;
    for (usize start = 0; start <= text.size();) {
        const usize comma = std::min(text.find(',', start), text.size());
        std::string id = trimmed(text.substr(start, comma - start));
        if (!id.empty() && id != kDefaultPack && std::ranges::find(packs, id) == packs.end()) {
            packs.push_back(std::move(id));
        }
        start = comma + 1;
    }
    return packs;
}

std::string joinPackList(const std::vector<std::string>& packs) {
    std::string text;
    for (const std::string& id : packs) {
        text += (text.empty() ? "" : ", ") + id;
    }
    return text;
}

void ResourcePacksScreen::build(vxe::ui::Widget& root) {
    using namespace vxe::ui;
    root.style().direction = Direction::Row;
    root.style().justify = Justify::Center;
    root.style().padding = Edges::all(8.0F);
    auto& panel = root.add<Panel>();
    panel.style().grow = 1.0F;
    panel.style().maxWidth = kPanelWidth;
    panel.style().gap = 4.0F;
    panel.add<Label>("Resource Packs", TextRole::Title).setAlign(TextAlign::Center);

    auto& lists = row(panel);
    lists.style().grow = 1.0F;
    m_availableList = &column(lists, "Available");
    m_enabledList = &column(lists, "Enabled, top wins");

    m_details = &panel.add<Label>("Select a pack to see what it is.", TextRole::Muted);
    m_details->setWrap(true);

    auto& actions = row(panel);
    m_toggle = &wideButton(actions, "Enable", [this] {
        std::vector<std::string> enabled = m_enabled;
        if (const auto found = std::ranges::find(enabled, m_selected); found != enabled.end()) {
            enabled.erase(found);
        } else {
            enabled.insert(enabled.begin(), m_selected); // A new pack goes on top.
        }
        setEnabled(std::move(enabled));
    });
    m_up = &wideButton(actions, "Move up", [this] {
        std::vector<std::string> enabled = m_enabled;
        const auto found = std::ranges::find(enabled, m_selected);
        if (found != enabled.end() && found != enabled.begin()) {
            std::iter_swap(found, found - 1);
            setEnabled(std::move(enabled));
        }
    });
    m_down = &wideButton(actions, "Move down", [this] {
        std::vector<std::string> enabled = m_enabled;
        const auto found = std::ranges::find(enabled, m_selected);
        if (found != enabled.end() && found + 1 != enabled.end()) {
            std::iter_swap(found, found + 1);
            setEnabled(std::move(enabled));
        }
    });

    auto& bottom = row(panel);
    auto& folder = wideButton(bottom, "Open Packs Folder", [this] {
        if (m_services.packs == nullptr) {
            return;
        }
        if (const auto opened = vxe::platform::Platform::openInFileBrowser(m_services.packs->userFolder());
            !opened) {
            VXE_LOG_WARN(vxe::log::core, "{}", opened.error().describe());
        }
    });
    folder.setTooltip("Copy a pack folder in here, then press Reload.");
    wideButton(bottom, "Reload", [this] {
        if (m_services.packs != nullptr) {
            m_services.packs->reload();
        }
        refresh();
    });
    wideButton(bottom, "Done", [this] { manager()->pop(); });
    refresh();
}

void ResourcePacksScreen::refresh() {
    m_packs.clear();
    m_default = {};
    m_default.id = std::string(kDefaultPack);
    m_default.name = "Default";
    if (m_services.packs != nullptr) {
        for (vxe::render::PackManifest& pack : m_services.packs->availablePacks()) {
            if (pack.id == kDefaultPack) {
                if (m_default.description.empty()) {
                    m_default = std::move(pack);
                }
            } else if (std::ranges::find(m_packs, pack.id, &vxe::render::PackManifest::id) == m_packs.end()) {
                m_packs.push_back(std::move(pack));
            }
        }
    }
    std::ranges::sort(m_packs, {}, &vxe::render::PackManifest::name);
    // Packs named in the setting but missing from the folder stay enabled:
    // copying the folder back brings them back.
    const auto value = m_services.settings.value(kPacksSetting);
    const std::string* text = value ? std::get_if<std::string>(&value.value()) : nullptr;
    m_enabled = parsePackList(text != nullptr ? *text : std::string());
    if (!m_selected.empty() && m_selected != kDefaultPack && manifest(m_selected) == nullptr &&
        std::ranges::find(m_enabled, m_selected) == m_enabled.end()) {
        m_selected.clear();
    }
    rebuildLists();
}

void ResourcePacksScreen::rebuildLists() {
    using namespace vxe::ui;
    m_rows.clear();
    m_availableList->clearChildren();
    m_enabledList->clearChildren();
    const auto addRow = [this](Widget& list, const std::string& id, std::string text) {
        auto& button = list.add<Button>(std::move(text), [this, id] { select(id); });
        button.setSelected(id == m_selected);
        m_rows.emplace_back(id, &button);
    };
    for (const vxe::render::PackManifest& pack : m_packs) {
        if (std::ranges::find(m_enabled, pack.id) == m_enabled.end()) {
            addRow(*m_availableList, pack.id, pack.name);
        }
    }
    if (m_availableList->children().empty()) {
        m_availableList->add<Label>("No other packs. Copy one into the packs folder.", TextRole::Muted)
            .setWrap(true);
    }
    for (const std::string& id : m_enabled) {
        const vxe::render::PackManifest* pack = manifest(id);
        addRow(*m_enabledList, id, pack != nullptr ? pack->name : std::format("{} (missing)", id));
    }
    addRow(*m_enabledList, m_default.id, m_default.name);
    select(m_selected);
}

const vxe::render::PackManifest* ResourcePacksScreen::manifest(std::string_view id) const {
    if (id == kDefaultPack) {
        return &m_default;
    }
    const auto found = std::ranges::find(m_packs, id, &vxe::render::PackManifest::id);
    return found == m_packs.end() ? nullptr : &*found;
}

void ResourcePacksScreen::select(const std::string& id) {
    m_selected = id;
    for (const auto& [rowId, button] : m_rows) {
        button->setSelected(rowId == m_selected);
    }
    if (m_selected.empty()) {
        m_details->setText("Select a pack to see what it is.");
    } else if (const vxe::render::PackManifest* pack = manifest(m_selected); pack != nullptr) {
        std::string text = pack->name;
        if (!pack->author.empty()) {
            text += std::format(" by {}", pack->author);
        }
        if (!pack->version.empty()) {
            text += std::format(", version {}", pack->version);
        }
        text += std::format(", {} x {} pixels.", pack->resolution, pack->resolution);
        if (!pack->description.empty()) {
            text += " " + pack->description;
        }
        if (m_selected == kDefaultPack) {
            text += " Always on, below every other pack.";
        }
        m_details->setText(std::move(text));
    } else {
        m_details->setText(std::format("'{}' is not in the packs folder.", m_selected));
    }
    updateButtons();
}

void ResourcePacksScreen::updateButtons() {
    const auto found = std::ranges::find(m_enabled, m_selected);
    const bool enabled = found != m_enabled.end();
    const bool movable = !m_selected.empty() && m_selected != kDefaultPack;
    m_toggle->setText(enabled ? "Disable" : "Enable");
    m_toggle->setEnabled(movable && (enabled || manifest(m_selected) != nullptr));
    m_up->setEnabled(enabled && found != m_enabled.begin());
    m_down->setEnabled(enabled && found + 1 != m_enabled.end());
}

void ResourcePacksScreen::setEnabled(std::vector<std::string> enabled) {
    const std::string text = joinPackList(enabled);
    if (const auto set = m_services.settings.set(kPacksSetting, vxe::config::SettingValue{text}); !set) {
        VXE_LOG_WARN(vxe::log::core, "Could not change the texture packs: {}", set.error().describe());
        return;
    }
    m_enabled = std::move(enabled);
    rebuildLists();
}

} // namespace vr
