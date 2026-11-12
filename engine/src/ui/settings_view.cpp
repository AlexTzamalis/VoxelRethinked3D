#include "vxe/ui/settings_view.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <format>
#include <map>
#include <variant>

namespace vxe::ui {
namespace {

// Top level groups, roughly in the order a player looks for them.
constexpr std::array<std::string_view, 11> kGroupOrder = {
    "Video",     "Graphics",      "World",       "Gameplay", "Controls", "Audio",
    "Interface", "Accessibility", "Performance", "Server",   "Debug"};

std::string lowered(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

// Which tag a setting's row shows, and why (the tag's tooltip).
std::pair<std::string_view, std::string_view> applyTag(config::ApplyMode mode) {
    switch (mode) {
    case config::ApplyMode::RequiresWorldReload:
        return {"Reload", "Applies after leaving and entering the world again."};
    case config::ApplyMode::RequiresRestart:
        return {"Restart", "Applies after restarting the game."};
    case config::ApplyMode::Live:
    case config::ApplyMode::RequiresRemesh:
    case config::ApplyMode::RequiresRendererReload:
        break; // Applied automatically, right away.
    }
    return {};
}

// A string field that stores on Enter and when it loses the focus.
class SettingTextField final : public TextField {
public:
    SettingTextField(std::string text, std::function<void(const std::string&)> store)
        : TextField(std::move(text))
        , m_store(std::move(store)) {
        setOnSubmit([this](const std::string& value) { m_store(value); });
    }
    void onFocus(bool focused) override {
        TextField::onFocus(focused);
        if (!focused) {
            m_store(text());
        }
    }

private:
    std::function<void(const std::string&)> m_store;
};

} // namespace

std::string spacedName(std::string_view name) {
    std::string result;
    for (usize index = 0; index < name.size(); ++index) {
        const auto c = static_cast<unsigned char>(name[index]);
        const bool upper = std::isupper(c) != 0;
        // A space before a capital that starts a word: "FpsLimit" but not "LOD".
        if (upper && index > 0 &&
            (std::islower(static_cast<unsigned char>(name[index - 1])) != 0 ||
             (index + 1 < name.size() && std::islower(static_cast<unsigned char>(name[index + 1])) != 0 &&
              std::isupper(static_cast<unsigned char>(name[index - 1])) != 0))) {
            result.push_back(' ');
        }
        result.push_back(static_cast<char>(c));
    }
    return result;
}

std::vector<std::string> orderedSettingGroups(const config::SettingsRegistry& settings) {
    std::vector<std::string> groups;
    for (const std::string& category : settings.categories()) {
        std::string group = settings.groupOf(category);
        if (std::ranges::find(groups, group) == groups.end()) {
            groups.push_back(std::move(group));
        }
    }
    const auto rank = [](const std::string& group) {
        const auto found = std::ranges::find(kGroupOrder, group);
        return static_cast<usize>(found - kGroupOrder.begin());
    };
    std::ranges::sort(groups, [&rank](const std::string& a, const std::string& b) {
        return rank(a) != rank(b) ? rank(a) < rank(b) : a < b;
    });
    return groups;
}

SettingsView::SettingsView(config::SettingsRegistry& settings, SettingsViewOptions options)
    : m_settings(settings)
    , m_options(std::move(options)) {
    style().direction = Direction::Column;
    style().gap = 4.0F;
    style().grow = 1.0F;
    for (const std::string& group : orderedSettingGroups(settings)) {
        // A group whose settings are all hidden gets no tab.
        const bool anyShown = std::ranges::any_of(m_settings.all(), [&](const config::SettingInfo* info) {
            return m_settings.groupOf(info->category) == group && shows(*info);
        });
        if (anyShown) {
            m_groups.push_back(group);
        }
    }
    m_tabs = &add<Widget>();
    m_tabs->style().direction = Direction::Row;
    m_tabs->style().gap = 2.0F;
    auto& body = add<Widget>();
    body.style().direction = Direction::Row;
    body.style().gap = 6.0F;
    body.style().grow = 1.0F;
    auto& pageScroll = body.add<ScrollView>();
    pageScroll.style().width = kPageListWidth;
    pageScroll.style().shrink = 0.0F;
    m_list = &pageScroll.add<Widget>();
    m_list->style().gap = 2.0F;
    auto& column = body.add<Widget>();
    column.style().grow = 1.0F;
    column.style().shrink = 1.0F;
    column.style().minWidth = 0.0F;
    column.style().gap = 3.0F;
    if (m_options.searchBox) {
        m_search = &column.add<TextField>(m_options.filter, "Search all settings");
        m_search->setOnChange([this](const std::string& text) { setFilter(text); });
    }
    m_scroll = &column.add<ScrollView>();
    m_scroll->style().grow = 1.0F;
    m_scroll->add<Widget>().style().gap = 3.0F;
    buildTabs();
    selectGroup(m_groups.empty() ? std::string() : m_groups.front());
}

bool SettingsView::shows(const config::SettingInfo& info) const {
    return m_options.showDeveloper || info.visibility != config::Visibility::Developer;
}

void SettingsView::buildTabs() {
    m_tabs->clearChildren();
    m_groupButtons.clear();
    // The group names never change after construction, so buttons may hold them.
    for (const std::string& group : m_groups) {
        const std::string* name = &group;
        auto& button = m_tabs->add<Button>(group, [this, name] { showGroup(*name); });
        button.style().grow = 1.0F;
        button.style().minWidth = 0.0F;
        m_groupButtons.emplace_back(group, &button);
    }
}

// A setting's page: its category, or its group's General page when the
// category is the group itself (Video.WindowMode lives on Video's first page).
std::string SettingsView::pageOf(const config::SettingInfo& info) const {
    return info.category;
}

std::string SettingsView::pageTitle(const std::string& page) const {
    if (page == m_group && m_settings.groupOf(page) == page) {
        return "General";
    }
    if (std::string name = m_settings.categoryName(page); !name.empty()) {
        return name;
    }
    return spacedName(std::string_view(page).substr(page.rfind('.') + 1));
}

void SettingsView::buildPageList() {
    m_pages.clear();
    const std::vector<std::string> featured = m_settings.featuredSettings(m_group);
    bool general = !featured.empty() || !m_settings.presetNames(m_group).empty();
    std::vector<std::string> others;
    for (const config::SettingInfo* info : m_settings.all()) {
        if (!shows(*info) || m_settings.groupOf(info->category) != m_group) {
            continue;
        }
        if (info->category == m_group) {
            general = true;
        } else if (std::ranges::find(others, info->category) == others.end()) {
            others.push_back(info->category);
        }
    }
    if (general) {
        m_pages.emplace_back(m_group, "General");
    }
    std::vector<std::pair<std::string, std::string>> named;
    for (const std::string& page : others) {
        named.emplace_back(page, pageTitle(page));
    }
    std::ranges::sort(named, {}, &std::pair<std::string, std::string>::second);
    m_pages.insert(m_pages.end(), named.begin(), named.end());

    m_list->clearChildren();
    m_pageButtons.clear();
    for (usize index = 0; index < m_pages.size(); ++index) {
        auto& button =
            m_list->add<Button>(m_pages[index].second, [this, index] { showPage(m_pages[index].first); });
        button.style().minWidth = 0.0F;
        m_pageButtons.emplace_back(m_pages[index].first, &button);
    }
}

void SettingsView::showGroup(std::string group) {
    m_options.filter.clear();
    if (m_search != nullptr) {
        m_search->setText({});
    }
    selectGroup(std::move(group));
}

void SettingsView::showPage(std::string page) {
    m_options.filter.clear();
    if (m_search != nullptr) {
        m_search->setText({});
    }
    selectPage(std::move(page));
}

void SettingsView::selectGroup(std::string group) {
    m_group = std::move(group);
    for (const auto& [name, button] : m_groupButtons) {
        button->setSelected(name == m_group);
    }
    buildPageList();
    selectPage(m_pages.empty() ? std::string() : m_pages.front().first);
}

void SettingsView::selectPage(std::string page) {
    m_page = std::move(page);
    for (const auto& [key, button] : m_pageButtons) {
        button->setSelected(key == m_page && m_options.filter.empty());
    }
    m_scroll->scrollTo(0.0F);
    buildPage();
}

void SettingsView::setFilter(std::string filter) {
    m_options.filter = std::move(filter);
    selectPage(m_page);
}
bool SettingsView::pageChanged() const {
    return std::ranges::any_of(m_rows,
                               [this](const Row& row) { return !m_settings.isDefault(row.info->id); });
}

void SettingsView::resetPage() {
    for (const Row& row : m_rows) {
        static_cast<void>(m_settings.reset(row.info->id));
    }
    refresh();
}

void SettingsView::buildPage() {
    Widget& page = *m_scroll->children().front();
    page.clearChildren();
    m_rows.clear();
    m_presetRows.clear();
    const std::string needle = lowered(m_options.filter);
    if (needle.empty()) {
        const bool general = !m_pages.empty() && m_page == m_pages.front().first && m_page == m_group;
        addPresetRow(page, m_page);
        if (general) {
            // The settings players look for most, wherever they live.
            for (const std::string& id : m_settings.featuredSettings(m_group)) {
                if (const config::SettingInfo* info = m_settings.find(id);
                    info != nullptr && shows(*info) && info->category != m_page) {
                    addRow(page, *info);
                }
            }
        }
        for (const config::SettingInfo* info : m_settings.all()) {
            if (shows(*info) && info->category == m_page) {
                addRow(page, *info);
            }
        }
        if (m_rows.empty()) {
            page.add<Label>("Nothing to set here.", TextRole::Muted);
        }
        refresh();
        return;
    }
    // A search: matches from every page, under a header naming where they live.
    std::vector<std::string> pageOrder;
    std::map<std::string, std::vector<const config::SettingInfo*>> byPage;
    for (const config::SettingInfo* info : m_settings.all()) {
        if (!shows(*info) || (lowered(info->displayName).find(needle) == std::string::npos &&
                              lowered(info->description).find(needle) == std::string::npos &&
                              lowered(info->id).find(needle) == std::string::npos)) {
            continue;
        }
        const std::string key = pageOf(*info);
        if (!byPage.contains(key)) {
            pageOrder.push_back(key);
        }
        byPage[key].push_back(info);
    }
    for (const std::string& key : pageOrder) {
        const std::string group = m_settings.groupOf(key);
        std::string title = group;
        if (key != group) {
            std::string name = m_settings.categoryName(key);
            title +=
                " / " + (name.empty() ? spacedName(std::string_view(key).substr(key.rfind('.') + 1)) : name);
        }
        auto& header = page.add<Label>(std::move(title), TextRole::Title);
        header.style().padding = Edges{0.0F, 4.0F, 0.0F, 1.0F};
        for (const config::SettingInfo* info : byPage[key]) {
            addRow(page, *info);
        }
    }
    if (m_rows.empty()) {
        page.add<Label>("No setting matches the search.", TextRole::Muted);
    }
    refresh();
}
void SettingsView::addPresetRow(Widget& page, const std::string& group) {
    std::vector<std::string> names = m_settings.presetNames(group);
    if (names.empty()) {
        return;
    }
    auto& row = page.add<Widget>();
    row.style().direction = Direction::Row;
    row.style().align = Align::Center;
    row.style().gap = 4.0F;
    auto& name = row.add<Label>(group == m_group ? "Quality preset" : "Preset");
    name.style().minWidth = 0.0F;
    name.style().grow = 1.0F;
    row.add<InfoBadge>("Sets the settings below together. Custom means they match no preset.");
    // The callback holds the row's index: rows live as long as the page.
    auto& cycle = row.add<CycleButton>(names, 0, [this, index = m_presetRows.size()](usize choice) {
        const PresetRow& preset = m_presetRows[index];
        static_cast<void>(m_settings.applyPreset(preset.group, preset.names[choice]));
    });
    cycle.style().width = kControlWidth;
    cycle.style().shrink = 0.0F;
    m_presetRows.push_back({.group = group, .names = std::move(names), .button = &cycle});
}

void SettingsView::addRow(Widget& page, const config::SettingInfo& info) {
    auto& row = page.add<Widget>();
    row.style().direction = Direction::Row;
    row.style().align = Align::Center;
    row.style().gap = 4.0F;
    auto& name = row.add<Label>(info.displayName.empty() ? spacedName(info.id) : info.displayName);
    name.style().grow = 1.0F;
    name.style().shrink = 1.0F;
    name.style().minWidth = 0.0F;
    // A long name may end in "...": its tooltip has the whole name and the details.
    name.setTooltip(info.description.empty() ? name.text() : name.text() + ": " + info.description);
    if (!info.description.empty()) {
        row.add<InfoBadge>(info.description).style().shrink = 0.0F;
    }
    if (const auto [tag, why] = applyTag(info.applyMode); !tag.empty()) {
        auto& label = row.add<Label>(std::string(tag), TextRole::Muted);
        label.setTooltip(std::string(why));
        label.style().shrink = 0.0F;
    }
    // Callbacks hold the registry's entry (its address never changes), not a
    // copy of the id.
    const config::SettingInfo* source = &info;
    Widget* control = nullptr;
    switch (info.type) {
    case config::SettingType::Bool: {
        auto& holder = row.add<Widget>();
        holder.style().width = kControlWidth;
        holder.style().shrink = 0.0F;
        holder.style().direction = Direction::Row;
        holder.style().align = Align::Center;
        control = &holder.add<Toggle>(false, [this, source](bool value) {
            static_cast<void>(m_settings.set(source->id, config::SettingValue{value}));
        });
        break;
    }
    case config::SettingType::Int:
    case config::SettingType::Float: {
        if (!info.choices.empty()) {
            std::vector<std::string> names;
            names.reserve(info.choices.size());
            for (const i32 choice : info.choices) {
                names.push_back(choice == 0 && !info.zeroName.empty() ? info.zeroName
                                                                      : std::to_string(choice));
            }
            auto& cycle = row.add<CycleButton>(std::move(names), 0, [this, source](usize index) {
                static_cast<void>(m_settings.set(source->id, config::SettingValue{source->choices[index]}));
            });
            cycle.style().width = kControlWidth;
            cycle.style().shrink = 0.0F;
            control = &cycle;
            break;
        }
        const bool whole = info.type == config::SettingType::Int;
        auto& slider =
            row.add<Slider>(info.min, info.max, whole ? std::max(1.0, info.step) : info.step, info.min,
                            [this, source, whole](f64 value) {
                                const config::SettingValue stored =
                                    whole ? config::SettingValue{static_cast<i32>(std::lround(value))}
                                          : config::SettingValue{static_cast<f32>(value)};
                                static_cast<void>(m_settings.set(source->id, stored));
                            });
        slider.style().width = kControlWidth;
        slider.style().shrink = 0.0F;
        control = &slider;
        break;
    }
    case config::SettingType::Enum: {
        std::vector<std::string> names;
        names.reserve(info.options.size());
        for (const config::EnumOption& option : info.options) {
            names.push_back(option.displayName.empty() ? option.name : option.displayName);
        }
        auto& cycle = row.add<CycleButton>(std::move(names), 0, [this, source](usize index) {
            static_cast<void>(m_settings.set(source->id, config::SettingValue{source->options[index].value}));
        });
        cycle.style().width = kControlWidth;
        cycle.style().shrink = 0.0F;
        control = &cycle;
        break;
    }
    case config::SettingType::String: {
        auto& field = row.add<SettingTextField>(std::string(), [this, source](const std::string& value) {
            const auto current = m_settings.value(source->id);
            if (!current || std::get<std::string>(current.value()) != value) {
                static_cast<void>(m_settings.set(source->id, config::SettingValue{value}));
            }
        });
        field.style().width = kControlWidth;
        field.style().shrink = 0.0F;
        if (info.maxLength > 0) {
            field.setMaxLength(info.maxLength);
        }
        control = &field;
        break;
    }
    }
    if (control != nullptr && !info.description.empty()) {
        control->setTooltip(info.description); // Keyboard users see it too.
    }
    auto& reset =
        row.add<Button>("Reset", [this, source] { static_cast<void>(m_settings.reset(source->id)); });
    reset.style().minWidth = 0.0F;
    reset.style().shrink = 0.0F;
    reset.setTooltip("Back to the default value.");
    m_rows.push_back({.info = &info, .control = control, .reset = &reset});
}

void SettingsView::refresh() {
    for (const PresetRow& preset : m_presetRows) {
        const std::string current = m_settings.currentPreset(preset.group);
        const auto found = std::ranges::find(preset.names, current);
        if (found == preset.names.end()) {
            preset.button->showOffList(current);
        } else {
            preset.button->setIndex(static_cast<usize>(found - preset.names.begin()));
        }
    }
    for (const Row& row : m_rows) {
        const auto value = m_settings.value(row.info->id);
        if (!value) {
            continue;
        }
        const config::SettingValue& current = value.value();
        switch (row.info->type) {
        case config::SettingType::Bool:
            dynamic_cast<Toggle&>(*row.control).setValue(std::get<bool>(current));
            break;
        case config::SettingType::Int:
        case config::SettingType::Float: {
            if (!row.info->choices.empty()) {
                // A value set by hand shows as the nearest choice.
                const i32 stored = std::get<i32>(current);
                const auto& choices = row.info->choices;
                usize nearest = 0;
                for (usize i = 1; i < choices.size(); ++i) {
                    if (std::abs(choices[i] - stored) < std::abs(choices[nearest] - stored)) {
                        nearest = i;
                    }
                }
                dynamic_cast<CycleButton&>(*row.control).setIndex(nearest);
                break;
            }
            auto* slider = dynamic_cast<Slider*>(row.control);
            if (!slider->dragging()) {
                slider->setValue(row.info->type == config::SettingType::Int
                                     ? static_cast<f64>(std::get<i32>(current))
                                     : static_cast<f64>(std::get<f32>(current)));
            }
            break;
        }
        case config::SettingType::Enum: {
            const i32 stored = std::get<i32>(current);
            const auto found = std::ranges::find(row.info->options, stored, &config::EnumOption::value);
            if (found != row.info->options.end()) {
                dynamic_cast<CycleButton&>(*row.control)
                    .setIndex(static_cast<usize>(found - row.info->options.begin()));
            }
            break;
        }
        case config::SettingType::String: {
            auto* field = dynamic_cast<TextField*>(row.control);
            if (!field->focused()) {
                field->setText(std::get<std::string>(current));
            }
            break;
        }
        }
        row.reset->setEnabled(!m_settings.isDefault(row.info->id));
    }
}

} // namespace vxe::ui
