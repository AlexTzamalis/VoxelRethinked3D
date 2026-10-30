#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/ui/controls.hpp>
#include <vxe/ui/widgets.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace vxe::ui {

struct SettingsViewOptions {
    // Developer settings (Visibility::Developer) show only when asked for.
    bool showDeveloper = false;
    // Shows only settings whose name, description or id contains this text
    // (any case), across every group; empty shows the chosen group.
    std::string filter{};
    // A search box above the settings that sets the filter while typing.
    bool searchBox = false;
};

// The settings UI generated from the SettingsRegistry (Section 16.1, E12.6):
// the top level groups on the left (Video, Graphics, Controls, ...), the
// chosen group's settings on the right in a scroll view, under a header per
// sub category. A row is the setting's name, an (i) with its description, a
// tag when a change only applies after a world reload or a restart, the
// control for its type (a switch, a slider, a choice, a text field) and a
// Reset button. A group or category with presets (Section 16.2) starts with a
// row that picks one and shows Custom when no preset matches. No setting
// needs hand written UI.
//
// Changes go to the registry at once (text fields on Enter or when they lose
// the focus). refresh() brings the controls up to date with changes made
// elsewhere (the console, presets), so call it every frame the view shows.
//
// Thread safety: main thread only, like the registry's writes.
class SettingsView : public Widget {
public:
    explicit SettingsView(config::SettingsRegistry& settings, SettingsViewOptions options = {});

    // The top level groups in display order.
    [[nodiscard]] const std::vector<std::string>& groups() const noexcept { return m_groups; }
    [[nodiscard]] const std::string& group() const noexcept { return m_group; }
    // Shows a group; this ends a search.
    void showGroup(std::string group);
    void setFilter(std::string filter);
    [[nodiscard]] const std::string& filter() const noexcept { return m_options.filter; }
    // The search box, null without SettingsViewOptions::searchBox.
    [[nodiscard]] TextField* searchBox() const noexcept { return m_search; }
    // Syncs every control with the registry's current values.
    void refresh();
    // How many settings rows the page shows (preset rows not counted).
    [[nodiscard]] usize rowCount() const noexcept { return m_rows.size(); }
    // Whether a shown setting differs from its default, and the action that
    // restores every shown setting (Section 19.3's "Reset category").
    [[nodiscard]] bool pageChanged() const;
    void resetPage();

    // The width of a row's control, in UI pixels.
    static constexpr f32 kControlWidth = 120.0F;
    static constexpr f32 kGroupListWidth = 92.0F;

private:
    struct Row {
        const config::SettingInfo* info = nullptr;
        Widget* control = nullptr;
        Button* reset = nullptr;
    };

    struct PresetRow {
        std::string group;
        std::vector<std::string> names;
        CycleButton* button = nullptr;
    };

    void buildGroupList();
    void selectGroup(std::string group);
    void buildPage();
    void addRow(Widget& page, const config::SettingInfo& info);
    void addPresetRow(Widget& page, const std::string& group);
    [[nodiscard]] bool shows(const config::SettingInfo& info) const;

    config::SettingsRegistry& m_settings;
    SettingsViewOptions m_options;
    std::vector<std::string> m_groups;
    std::string m_group;
    Widget* m_list = nullptr;
    TextField* m_search = nullptr;
    ScrollView* m_scroll = nullptr;
    std::vector<Row> m_rows;
    std::vector<PresetRow> m_presetRows;
    std::vector<std::pair<std::string, Button*>> m_groupButtons;
};

// The order of the top level groups in the settings UI; groups not listed
// follow alphabetically.
[[nodiscard]] std::vector<std::string> orderedSettingGroups(const config::SettingsRegistry& settings);
// A label from a name in an id: "LodQuality" becomes "Lod Quality".
[[nodiscard]] std::string spacedName(std::string_view name);

} // namespace vxe::ui
