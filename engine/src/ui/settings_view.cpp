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
    "Video",         "Graphics", "World",       "Gameplay", "Controls", "Interface",
    "Accessibility", "Audio",    "Performance", "Server",   "Debug"};

std::string_view topGroup(std::string_view category) {
    return category.substr(0, category.find('.'));
}

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
        const std::string group(topGroup(category));
        if (std::ranges::find(groups, group) == groups.end()) {
            groups.push_back(group);
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
    , m_options(std::move(options))
    , m_list(&add<Widget>())
    , m_scroll(&add<ScrollView>()) {
    style().direction = Direction::Row;
    style().gap = 6.0F;
    style().grow = 1.0F;
    for (const std::string& group : orderedSettingGroups(settings)) {
        // A group whose settings are all hidden gets no button.
        const bool anyShown = std::ranges::any_of(m_settings.all(), [&](const config::SettingInfo* info) {
            return topGroup(info->category) == group && shows(*info);
        });
        if (anyShown) {
            m_groups.push_back(group);
        }
    }
    m_list->style().width = kGroupListWidth;
    m_list->style().gap = 2.0F;
    m_scroll->style().grow = 1.0F;
    m_scroll->add<Widget>().style().gap = 3.0F;
    buildGroupList();
    showGroup(m_groups.empty() ? std::string() : m_groups.front());
}

bool SettingsView::shows(const config::SettingInfo& info) const {
    return m_options.showDeveloper || info.visibility != config::Visibility::Developer;
}

void SettingsView::buildGroupList() {
    m_list->clearChildren();
    m_groupButtons.clear();
    // The group names never change after construction, so buttons may hold them.
    for (const std::string& group : m_groups) {
        const std::string* name = &group;
        auto& button = m_list->add<Button>(group, [this, name] { showGroup(*name); });
        button.style().minWidth = 0.0F;
        m_groupButtons.emplace_back(group, &button);
    }
}

void SettingsView::showGroup(std::string group) {
    m_group = std::move(group);
    for (const auto& [name, button] : m_groupButtons) {
        button->setSelected(name == m_group && m_options.filter.empty());
    }
    m_scroll->scrollTo(0.0F);
    buildPage();
}

void SettingsView::setFilter(std::string filter) {
    m_options.filter = std::move(filter);
    showGroup(m_group);
}

void SettingsView::buildPage() {
    Widget& page = *m_scroll->children().front();
    page.clearChildren();
    m_rows.clear();
    const std::string needle = lowered(m_options.filter);
    // Settings by category, in the order they were registered.
    std::map<std::string, std::vector<const config::SettingInfo*>> byCategory;
    std::vector<std::string> categoryOrder;
    for (const config::SettingInfo* info : m_settings.all()) {
        if (!shows(*info)) {
            continue;
        }
        if (needle.empty()) {
            if (topGroup(info->category) != m_group) {
                continue;
            }
        } else if (lowered(info->displayName).find(needle) == std::string::npos &&
                   lowered(info->description).find(needle) == std::string::npos &&
                   lowered(info->id).find(needle) == std::string::npos) {
            continue;
        }
        if (!byCategory.contains(info->category)) {
            categoryOrder.push_back(info->category);
        }
        byCategory[info->category].push_back(info);
    }
    // The group's own settings first, then its sub categories by name.
    std::ranges::stable_sort(categoryOrder, [](const std::string& a, const std::string& b) {
        const bool aTop = a.find('.') == std::string::npos;
        const bool bTop = b.find('.') == std::string::npos;
        return aTop != bTop ? aTop : a < b;
    });
    for (const std::string& category : categoryOrder) {
        const bool topLevel = category.find('.') == std::string::npos;
        if (!topLevel || !needle.empty()) {
            const std::string_view name = needle.empty()
                                              ? std::string_view(category).substr(category.find('.') + 1)
                                              : std::string_view(category);
            std::string title;
            for (const char c : name) {
                title += c == '.' ? std::string(" / ") : std::string(1, c);
            }
            auto& header = page.add<Label>(spacedName(title), TextRole::Title);
            header.style().padding = Edges{0.0F, 4.0F, 0.0F, 1.0F};
        }
        for (const config::SettingInfo* info : byCategory[category]) {
            addRow(page, *info);
        }
    }
    if (m_rows.empty()) {
        page.add<Label>(needle.empty() ? "Nothing to set here." : "No setting matches the search.",
                        TextRole::Muted);
    }
    refresh();
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
