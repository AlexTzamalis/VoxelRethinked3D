#include "world_screens.hpp"

#include <vxe/core/log.hpp>
#include <vxe/core/version.hpp>
#include <vxe/platform/platform.hpp>
#include <vxe/ui/dialog.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <format>
#include <string_view>
#include <utility>

namespace vr {
namespace {

using vxe::f32;
using vxe::i32;
using vxe::i64;
using vxe::usize;

constexpr f32 kPanelWidth = 480.0F;
constexpr f32 kDoubleClickSeconds = 0.4F;
constexpr f32 kRowLabelWidth = 120.0F;
constexpr usize kMaxWorldName = 40;
constexpr usize kMaxSeedLength = 40;

constexpr std::array<std::string_view, 5> kWorldTypes = {"Default", "Amplified", "Flat", "Islands",
                                                         "Debug Grid"};
constexpr std::array<std::string_view, 3> kHeights = {"Tall", "Extreme", "Custom"};
constexpr std::array<std::string_view, 6> kPremaps = {"Off",        "4k blocks",  "8k blocks",
                                                      "16k blocks", "32k blocks", "Whole horizon"};
constexpr std::array<i32, 6> kPremapBlocks = {0, 4096, 8192, 16384, 32768, -1};
constexpr usize kPremapWhole = 5;
// The far terrain presets of Graphics.LOD, and Custom for hand made settings.
constexpr std::array<std::string_view, 5> kQualities = {"Low", "Medium", "High", "Ultra", "Custom"};
constexpr usize kQualityChoices = 4; // Custom only shows for worlds that already have it.
constexpr std::array<std::string_view, 1> kPresets = {"Default"};

template <usize Count>
std::vector<std::string> names(const std::array<std::string_view, Count>& list, usize count = Count) {
    return {list.begin(), list.begin() + static_cast<std::ptrdiff_t>(std::min(count, Count))};
}

// Where a stored value sits in a list of choices, or `fallback`.
template <typename T, usize Count, typename V>
usize indexOf(const std::array<T, Count>& list, const V& value, usize fallback) {
    const auto found = std::ranges::find(list, value);
    return found == list.end() ? fallback : static_cast<usize>(found - list.begin());
}

i64 nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string dateText(i64 seconds) {
    if (seconds == 0) {
        return "Never";
    }
    const auto time = static_cast<std::time_t>(seconds);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    return std::format("{}-{:02}-{:02} {:02}:{:02}", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                       local.tm_hour, local.tm_min);
}

std::string sizeText(vxe::u64 bytes) {
    constexpr vxe::u64 kKilo = 1024;
    if (bytes < kKilo * kKilo) {
        return std::format("{} KB", std::max<vxe::u64>(bytes / kKilo, 1));
    }
    return std::format("{} MB", bytes / (kKilo * kKilo));
}

std::string lowered(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

// One centered panel filling the screen up to its maximum width.
vxe::ui::Widget& centeredPanel(vxe::ui::Widget& root, std::string title) {
    using namespace vxe::ui;
    root.style().direction = Direction::Row;
    root.style().justify = Justify::Center;
    root.style().padding = Edges::all(8.0F);
    auto& panel = root.add<Panel>();
    panel.style().grow = 1.0F;
    panel.style().maxWidth = kPanelWidth;
    panel.add<Label>(std::move(title), TextRole::Title).setAlign(TextAlign::Center);
    return panel;
}

vxe::ui::Widget& buttonRow(vxe::ui::Widget& parent) {
    using namespace vxe::ui;
    auto& row = parent.add<Widget>();
    row.style().direction = Direction::Row;
    row.style().gap = 4.0F;
    return row;
}

// A row of a form: a name on the left and a control on the right.
vxe::ui::Widget& formRow(vxe::ui::Widget& parent, std::string name) {
    using namespace vxe::ui;
    auto& row = buttonRow(parent);
    row.style().align = Align::Center;
    auto& label = row.add<Label>(std::move(name));
    label.style().width = kRowLabelWidth;
    label.style().shrink = 0.0F;
    return row;
}

vxe::ui::Button& wideButton(vxe::ui::Widget& row, std::string text, std::function<void()> onClick) {
    auto& button = row.add<vxe::ui::Button>(std::move(text), std::move(onClick));
    button.style().grow = 1.0F;
    button.style().minWidth = 0.0F;
    return button;
}

} // namespace

// World list.

void WorldSelectScreen::build(vxe::ui::Widget& root) {
    using namespace vxe::ui;
    auto& panel = centeredPanel(root, "Singleplayer");
    panel.style().gap = 4.0F;
    m_search = &panel.add<TextField>(std::string(), "Search worlds");
    m_search->setOnChange([this](const std::string& /*text*/) { rebuildList(); });
    auto& scroll = panel.add<ScrollView>();
    scroll.style().grow = 1.0F;
    m_list = &scroll.add<Widget>();
    m_list->style().gap = 2.0F;

    auto& top = buttonRow(panel);
    m_playButton = &wideButton(top, "Play", [this] {
        if (const WorldInfo* world = selected(); world != nullptr) {
            m_play(*world);
        }
    });
    wideButton(top, "Create New World", [this] {
        m_reloadOnReturn = true;
        manager()->push(std::make_unique<CreateWorldScreen>(m_services, m_play));
    });
    auto& middle = buttonRow(panel);
    m_recreateButton = &wideButton(middle, "Re-create", [this] {
        if (const WorldInfo* world = selected(); world != nullptr) {
            m_reloadOnReturn = true;
            manager()->push(std::make_unique<CreateWorldScreen>(m_services, m_play, world));
        }
    });
    m_recreateButton->setTooltip("A new world with this one's seed and options. This one stays.");
    auto& folder = wideButton(middle, "Open Folder", [this] {
        const WorldInfo* world = selected();
        const std::filesystem::path path =
            world != nullptr ? m_services.saves / world->folder : m_services.saves;
        if (const auto opened = vxe::platform::Platform::openInFileBrowser(path); !opened) {
            VXE_LOG_WARN(vxe::log::core, "{}", opened.error().describe());
        }
    });
    folder.setTooltip("Shows the selected world's folder, or the folder of all worlds.");
    auto& import = wideButton(middle, "Import World", [this] {
        if (m_services.platform == nullptr || m_task.running()) {
            return;
        }
        // The screen may close before the player picks a file.
        m_services.platform->showFileDialog(
            m_services.window,
            {.filters = {{.name = "World (zip)", .pattern = "zip"}},
             .defaultLocation = vxe::platform::Platform::downloadsFolder()},
            [this, life = std::weak_ptr<int>(m_life)](std::optional<std::filesystem::path> chosen) {
                if (!life.expired() && chosen) {
                    importFrom(*chosen);
                }
            });
    });
    import.setTooltip("Adds a world from a .zip file, such as one made with Export in Edit World.");
    auto& bottom = buttonRow(panel);
    m_editButton = &wideButton(bottom, "Edit", [this] {
        if (const WorldInfo* world = selected(); world != nullptr) {
            m_reloadOnReturn = true;
            manager()->push(std::make_unique<EditWorldScreen>(m_services, *world));
        }
    });
    m_deleteButton = &wideButton(bottom, "Delete", [this] {
        const WorldInfo* world = selected();
        if (world == nullptr) {
            return;
        }
        showDialog(m_services.ui,
                   {.title = "Delete this world?",
                    .message = std::format("'{}' will be lost forever.", world->name),
                    .buttons = {{.text = "Delete",
                                 .onClick = [this, folder = world->folder] { removeWorld(folder); }},
                                {.text = "Cancel", .cancel = true}}});
    });
    wideButton(bottom, "Back", [this] { manager()->pop(); });
    updateButtons();
    load();
}

void WorldSelectScreen::load() {
    auto loaded = std::make_shared<std::vector<WorldInfo>>();
    m_loaded = loaded;
    m_task.start(m_services.jobs, [loaded, saves = m_services.saves] { *loaded = scanWorlds(saves); });
}

void WorldSelectScreen::importFrom(const std::filesystem::path& zip) {
    if (m_task.running()) {
        return;
    }
    auto loaded = std::make_shared<std::vector<WorldInfo>>();
    m_loaded = loaded;
    auto result = std::make_shared<std::pair<std::string, std::string>>(); // Folder, or an error.
    m_imported = result;
    VXE_LOG_INFO(vxe::log::core, "Importing a world from {}", zip.string());
    m_task.start(m_services.jobs, [loaded, result, zip, saves = m_services.saves] {
        if (auto imported = importWorld(saves, zip); imported) {
            result->first = imported.value().folder;
        } else {
            result->second = imported.error().message();
        }
        *loaded = scanWorlds(saves);
    });
}

// The dialog names one world, so it hands over that world's folder: the
// selection may have changed since it opened.
void WorldSelectScreen::removeWorld(const std::string& folder) {
    if (m_task.running()) {
        return;
    }
    auto loaded = std::make_shared<std::vector<WorldInfo>>();
    m_loaded = loaded;
    VXE_LOG_INFO(vxe::log::core, "Deleting world folder '{}'", folder);
    auto target = std::make_shared<std::pair<std::filesystem::path, std::string>>(m_services.saves, folder);
    m_task.start(m_services.jobs, [loaded, target] {
        const auto& [saves, name] = *target;
        const auto deleted = deleteWorld(saves, name);
        *loaded = scanWorlds(saves);
        if (!deleted) {
            VXE_LOG_WARN(vxe::log::core, "Could not delete world: {}", deleted.error().describe());
        }
    });
    if (folder == m_selected) {
        m_selected.clear();
    }
    updateButtons();
}

void WorldSelectScreen::update(f32 deltaSeconds) {
    m_sinceClick += deltaSeconds;
    if (m_task.finished()) {
        m_worlds = std::move(*m_loaded);
        rebuildList();
        if (const auto imported = std::exchange(m_imported, nullptr); imported != nullptr) {
            if (!imported->second.empty()) {
                showDialog(m_services.ui, {.title = "Cannot import this world",
                                           .message = imported->second,
                                           .buttons = {{.text = "OK", .cancel = true}}});
            } else {
                select(imported->first);
            }
        }
    }
    if (m_reloadOnReturn && manager()->top() == this) {
        m_reloadOnReturn = false;
        load();
    }
}

void WorldSelectScreen::rebuildList() {
    using namespace vxe::ui;
    m_list->clearChildren();
    m_rows.clear();
    const std::string needle = lowered(m_search->text());
    for (const WorldInfo& world : m_worlds) {
        if (!needle.empty() && lowered(world.name).find(needle) == std::string::npos) {
            continue;
        }
        auto& row = buttonRow(*m_list);
        row.style().align = Align::Center;
        auto& button = wideButton(row, world.name, [this, folder = world.folder] { select(folder); });
        button.setSelected(world.folder == m_selected);
        auto& details = row.add<Label>(std::format("{}   {}   {}", dateText(world.lastPlayedSeconds),
                                                   world.worldType, sizeText(world.sizeBytes)),
                                       TextRole::Muted);
        details.style().shrink = 0.0F;
        m_rows.emplace_back(world.folder, &button);
    }
    if (m_rows.empty()) {
        m_list->add<Label>(m_worlds.empty() ? "No worlds yet. Create one!" : "No world matches the search.",
                           TextRole::Muted);
    }
    updateButtons();
}

void WorldSelectScreen::select(const std::string& folder) {
    if (folder == m_selected && m_sinceClick < kDoubleClickSeconds) {
        if (const WorldInfo* world = selected(); world != nullptr) {
            m_play(*world);
        }
        return;
    }
    m_sinceClick = 0.0F;
    m_selected = folder;
    for (const auto& [rowFolder, button] : m_rows) {
        button->setSelected(rowFolder == m_selected);
    }
    updateButtons();
}

const WorldInfo* WorldSelectScreen::selected() const {
    const auto found = std::ranges::find(m_worlds, m_selected, &WorldInfo::folder);
    return found == m_worlds.end() ? nullptr : &*found;
}

void WorldSelectScreen::updateButtons() {
    const bool any = selected() != nullptr;
    m_playButton->setEnabled(any);
    m_editButton->setEnabled(any);
    m_recreateButton->setEnabled(any);
    m_deleteButton->setEnabled(any);
}

// Create World.

CreateWorldScreen::CreateWorldScreen(const MenuServices& services, PlayWorld play, const WorldInfo* from)
    : m_services(services)
    , m_play(std::move(play)) {
    if (from == nullptr) {
        return;
    }
    m_recreating = true;
    m_name = from->name;
    m_seed = from->seedText;
    m_type = indexOf(kWorldTypes, from->worldType, 0);
    m_preset = indexOf(kPresets, from->preset, 0);
    m_structures = from->structures;
    m_premap = indexOf(kPremapBlocks, from->premapBlocks, kPremapWhole);
    m_quality = indexOf(kQualities, from->lodQuality, 2);
    if (m_quality >= kQualityChoices) {
        m_quality = 2; // Custom settings belong to the old world, not to its options.
    }
    if (from->minY == -1024 && from->maxY == 3072) {
        m_height = 0;
    } else if (from->minY == -8192 && from->maxY == 16384) {
        m_height = 1;
    } else {
        m_height = 2;
        m_minY = std::to_string(from->minY);
        m_maxY = std::to_string(from->maxY);
    }
}

void CreateWorldScreen::build(vxe::ui::Widget& root) {
    using namespace vxe::ui;
    auto& panel = centeredPanel(root, m_recreating ? "Re-create World" : "Create New World");
    panel.style().gap = 4.0F;
    auto& tabs = buttonRow(panel);
    const auto addTab = [&](std::string text, Tab tab) {
        auto& button = wideButton(tabs, std::move(text), [this, tab] { showTab(tab); });
        m_tabs.emplace_back(tab, &button);
    };
    addTab("Game", Tab::Game);
    addTab("World", Tab::World);
    addTab("Far Terrain", Tab::Lod);
    addTab("More", Tab::More);
    m_page = &panel.add<Widget>();
    m_page->style().grow = 1.0F;
    m_page->style().gap = 4.0F;
    auto& buttons = buttonRow(panel);
    m_createButton = &wideButton(buttons, "Create World", [this] { create(); });
    wideButton(buttons, "Cancel", [this] { manager()->pop(); });
    showTab(Tab::Game);
}

void CreateWorldScreen::showTab(Tab tab) {
    m_tab = tab;
    for (const auto& [buttonTab, button] : m_tabs) {
        button->setSelected(buttonTab == tab);
    }
    buildPage();
}

void CreateWorldScreen::buildPage() {
    using namespace vxe::ui;
    m_page->clearChildren();
    const auto addChoice = [this](std::string name, std::vector<std::string> choices, usize& value,
                                  std::function<void()> changed = {}) {
        auto& row = formRow(*m_page, std::move(name));
        auto& cycle = row.add<CycleButton>(std::move(choices), value,
                                           [&value, changed = std::move(changed)](usize index) {
                                               value = index;
                                               if (changed) {
                                                   changed();
                                               }
                                           });
        cycle.style().grow = 1.0F;
    };
    const auto addText = [this](std::string name, std::string& value, std::string placeholder,
                                usize maxLength) -> TextField& {
        auto& row = formRow(*m_page, std::move(name));
        auto& field = row.add<TextField>(value, std::move(placeholder));
        field.style().grow = 1.0F;
        field.style().minWidth = 0.0F;
        field.setMaxLength(maxLength);
        field.setOnChange([&value](const std::string& text) { value = text; });
        return field;
    };
    switch (m_tab) {
    case Tab::Game: {
        addText("World name", m_name, "New World", kMaxWorldName);
        auto& row = formRow(*m_page, "Game mode");
        auto& mode = row.add<Label>("Creative");
        mode.setTooltip("Survival is not part of the game yet.");
        break;
    }
    case Tab::World: {
        auto& seed = addText("Seed", m_seed, "Random", kMaxSeedLength);
        seed.setTooltip("A number, or any text. Empty picks a random seed.");
        // The field is the last child of the row the helper just made.
        auto& seedRow = *m_page->children().back();
        auto& random = seedRow.add<Button>("Random", [this, &seed] {
            m_seed = randomSeedText();
            seed.setText(m_seed);
        });
        random.style().minWidth = 0.0F;
        addChoice("World type", names(kWorldTypes), m_type);
        addChoice("Height", names(kHeights), m_height, [this] { buildPage(); });
        if (m_height == 2) {
            const auto digitsOnly = [](char32_t c) { return (c >= U'0' && c <= U'9') || c == U'-'; };
            addText("Min Y", m_minY, "-1024", 7).setFilter(digitsOnly);
            addText("Max Y", m_maxY, "3072", 7).setFilter(digitsOnly);
        } else {
            m_page->add<Label>(m_height == 0 ? "From -1024 to 3072, 4096 blocks tall."
                                             : "From -8192 to 16384, 24576 blocks tall.",
                               TextRole::Muted);
        }
        auto& row = formRow(*m_page, "Generate structures");
        row.add<Toggle>(m_structures, [this](bool value) { m_structures = value; });
        m_page
            ->add<Label>("The seed shapes the terrain. Type and height come with the new world generator.",
                         TextRole::Muted)
            .setWrap(true);
        break;
    }
    case Tab::Lod:
        addChoice("Pre-map distance", names(kPremaps), m_premap);
        addChoice("Far terrain quality", names(kQualities, kQualityChoices), m_quality);
        m_page
            ->add<Label>(
                "Pre-mapping bakes far terrain before the world shows, so the horizon is there "
                "from the first moment. Off enters soonest, and far terrain fills in while you play.",
                TextRole::Muted)
            .setWrap(true);
        break;
    case Tab::More:
        addChoice("Worldgen preset", names(kPresets), m_preset);
        break;
    }
}

void CreateWorldScreen::create() {
    if (m_task.running()) {
        return;
    }
    const auto problem = [this](std::string message) {
        showDialog(m_services.ui, {.title = "Cannot create this world",
                                   .message = std::move(message),
                                   .buttons = {{.text = "OK", .cancel = true}}});
    };
    if (m_name.find_first_not_of(' ') == std::string::npos) {
        problem("The world needs a name.");
        return;
    }
    i32 minY = m_height == 1 ? -8192 : -1024;
    i32 maxY = m_height == 1 ? 16384 : 3072;
    if (m_height == 2) {
        const auto [minEnd, minError] = std::from_chars(m_minY.data(), m_minY.data() + m_minY.size(), minY);
        const auto [maxEnd, maxError] = std::from_chars(m_maxY.data(), m_maxY.data() + m_maxY.size(), maxY);
        if (minError != std::errc() || maxError != std::errc()) {
            problem("Min Y and Max Y must be whole numbers.");
            return;
        }
    }
    if (const auto valid = validateWorldHeight(minY, maxY); !valid) {
        problem(valid.error().message());
        return;
    }
    auto info = std::make_shared<WorldInfo>();
    info->name = m_name;
    info->seedText = m_seed.empty() ? randomSeedText() : m_seed;
    info->seed = seedFromText(info->seedText);
    info->worldType = std::string(kWorldTypes.at(m_type));
    info->preset = std::string(kPresets.at(m_preset));
    info->minY = minY;
    info->maxY = maxY;
    info->structures = m_structures;
    info->premapBlocks = kPremapBlocks.at(m_premap);
    info->lodQuality = std::string(kQualities.at(m_quality));
    info->createdSeconds = nowSeconds();
    info->lastPlayedSeconds = info->createdSeconds;
    info->gameVersion = std::string(vxe::engineVersion());
    auto error = std::make_shared<std::string>();
    m_created = info;
    m_error = error;
    m_createButton->setEnabled(false);
    m_task.start(m_services.jobs, [info, error, saves = m_services.saves] {
        info->folder = uniqueWorldFolder(saves, info->name);
        if (const auto saved = saveWorldInfo(saves, *info); !saved) {
            *error = saved.error().message();
        }
    });
}

void CreateWorldScreen::update(f32 /*deltaSeconds*/) {
    if (!m_task.finished()) {
        return;
    }
    m_createButton->setEnabled(true);
    if (!m_error->empty()) {
        showDialog(m_services.ui, {.title = "Cannot create this world",
                                   .message = *m_error,
                                   .buttons = {{.text = "OK", .cancel = true}}});
        return;
    }
    m_play(*m_created);
}

// Edit World.

void EditWorldScreen::build(vxe::ui::Widget& root) {
    using namespace vxe::ui;
    auto& panel = centeredPanel(root, "Edit World");
    panel.style().gap = 4.0F;
    auto& nameRow = formRow(panel, "World name");
    m_name = &nameRow.add<TextField>(m_info.name, "World name");
    m_name->style().grow = 1.0F;
    m_name->style().minWidth = 0.0F;
    m_name->setMaxLength(kMaxWorldName);
    // Far terrain options can change at any time: they only decide how the
    // world loads and looks, never what it is.
    const auto addChoice = [&panel](std::string name, std::vector<std::string> choices, usize& value) {
        auto& cycle =
            formRow(panel, std::move(name))
                .add<CycleButton>(std::move(choices), value, [&value](usize index) { value = index; });
        cycle.style().grow = 1.0F;
    };
    m_premap = indexOf(kPremapBlocks, m_info.premapBlocks, kPremapWhole);
    m_quality = indexOf(kQualities, m_info.lodQuality, 2);
    addChoice("Pre-map distance", names(kPremaps), m_premap);
    addChoice("Far terrain quality",
              names(kQualities, m_quality >= kQualityChoices ? kQualities.size() : kQualityChoices),
              m_quality);
    const auto addInfo = [&panel](std::string name, std::string value) {
        formRow(panel, std::move(name)).add<Label>(std::move(value), TextRole::Muted);
    };
    addInfo("Seed", m_info.seedText);
    addInfo("World type", m_info.worldType);
    addInfo("Height", std::format("{} to {}", m_info.minY, m_info.maxY));
    addInfo("Created", dateText(m_info.createdSeconds));
    addInfo("Last played", dateText(m_info.lastPlayedSeconds));
    addInfo("Size", sizeText(m_info.sizeBytes));
    panel.add<Widget>().style().grow = 1.0F;
    auto& buttons = buttonRow(panel);
    wideButton(buttons, "Save", [this] {
        if (m_task.running() || m_name->text().find_first_not_of(' ') == std::string::npos) {
            return;
        }
        m_info.name = m_name->text();
        m_info.premapBlocks = kPremapBlocks.at(m_premap);
        m_info.lodQuality = std::string(kQualities.at(m_quality));
        m_task.start(m_services.jobs, [info = m_info, saves = m_services.saves] {
            static_cast<void>(saveWorldInfo(saves, info));
        });
    });
    wideButton(buttons, "Cancel", [this] { manager()->pop(); });
    auto& exportButton = wideButton(buttons, "Export", [this] {
        if (m_services.platform == nullptr || m_task.running()) {
            return;
        }
        m_services.platform->showFileDialog(
            m_services.window,
            {.save = true,
             .filters = {{.name = "World (zip)", .pattern = "zip"}},
             .defaultLocation = vxe::platform::Platform::downloadsFolder() / (m_info.folder + ".zip")},
            [this, life = std::weak_ptr<int>(m_life)](std::optional<std::filesystem::path> chosen) {
                if (life.expired() || !chosen) {
                    return;
                }
                if (chosen->extension() != ".zip") {
                    chosen->replace_extension(".zip");
                }
                auto result = std::make_shared<std::string>();
                m_exported = result;
                m_exportPath = *chosen;
                m_task.start(m_services.jobs,
                             [result, path = *chosen, folder = m_info.folder, saves = m_services.saves] {
                                 if (const auto packed = exportWorld(saves, folder, path); !packed) {
                                     *result = packed.error().message();
                                 }
                             });
            });
    });
    exportButton.setTooltip(
        "Packs this world into a .zip file to keep or share. The far terrain is left out; "
        "it rebuilds when the world is played.");
}

void EditWorldScreen::update(f32 /*deltaSeconds*/) {
    if (!m_task.finished()) {
        return;
    }
    const auto exported = std::exchange(m_exported, nullptr);
    if (exported == nullptr) {
        manager()->pop(); // Saved.
        return;
    }
    if (!exported->empty()) {
        showDialog(m_services.ui, {.title = "Cannot export this world",
                                   .message = *exported,
                                   .buttons = {{.text = "OK", .cancel = true}}});
        return;
    }
    showDialog(m_services.ui,
               {.title = "World exported",
                .message = std::format("Saved as {}", m_exportPath.filename().string()),
                .buttons = {{.text = "Show in folder",
                             .onClick =
                                 [path = m_exportPath] {
                                     static_cast<void>(
                                         vxe::platform::Platform::openInFileBrowser(path.parent_path()));
                                 }},
                            {.text = "OK", .cancel = true}}});
}

} // namespace vr
