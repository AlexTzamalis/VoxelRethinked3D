#include "statistics_screen.hpp"

#include <vxe/ui/controls.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <vector>

namespace vr {
namespace {

using vxe::f32;

constexpr f32 kPanelWidth = 420.0F;
constexpr f32 kCountWidth = 64.0F;

// A row: a name on the left and values at fixed widths on the right.
void addRow(vxe::ui::Widget& page, std::string name, std::vector<std::string> values,
            vxe::ui::TextRole role = vxe::ui::TextRole::Normal) {
    using namespace vxe::ui;
    auto& row = page.add<Widget>();
    row.style().direction = Direction::Row;
    row.style().gap = 4.0F;
    auto& label = row.add<Label>(std::move(name), role);
    label.style().grow = 1.0F;
    label.style().minWidth = 0.0F;
    for (std::string& value : values) {
        auto& cell = row.add<Label>(std::move(value), role);
        cell.style().width = kCountWidth;
        cell.style().shrink = 0.0F;
        cell.setAlign(TextAlign::End);
    }
}

} // namespace

std::string playTimeText(vxe::f64 seconds) {
    const auto whole = static_cast<vxe::u64>(std::max(seconds, 0.0));
    if (whole < 60) {
        return std::format("{} s", whole);
    }
    if (whole < 3600) {
        return std::format("{} min", whole / 60);
    }
    return std::format("{} h {:02} min", whole / 3600, (whole / 60) % 60);
}

std::string distanceText(vxe::f64 blocks) {
    if (blocks < 1000.0) {
        return std::format("{} m", static_cast<vxe::u64>(std::max(blocks, 0.0)));
    }
    return std::format("{:.1f} km", blocks / 1000.0);
}

void StatisticsScreen::build(vxe::ui::Widget& root) {
    using namespace vxe::ui;
    root.style().direction = Direction::Row;
    root.style().justify = Justify::Center;
    root.style().padding = Edges::all(8.0F);
    auto& panel = root.add<Panel>();
    panel.style().grow = 1.0F;
    panel.style().maxWidth = kPanelWidth;
    panel.style().gap = 4.0F;
    panel.add<Label>("Statistics", TextRole::Title).setAlign(TextAlign::Center);
    auto& tabs = panel.add<Widget>();
    tabs.style().direction = Direction::Row;
    tabs.style().gap = 4.0F;
    m_generalTab = &tabs.add<Button>("General", [this] { showGeneral(); });
    m_generalTab->style().grow = 1.0F;
    m_blocksTab = &tabs.add<Button>("Blocks", [this] { showBlocks(); });
    m_blocksTab->style().grow = 1.0F;
    auto& scroll = panel.add<ScrollView>();
    scroll.style().grow = 1.0F;
    m_page = &scroll.add<Widget>();
    m_page->style().gap = 3.0F;
    auto& done = panel.add<Button>("Done", [this] { manager()->pop(); });
    done.style().alignSelf = Align::Center;
    done.style().width = 120.0F;
    showGeneral();
}

void StatisticsScreen::showGeneral() {
    m_generalTab->setSelected(true);
    m_blocksTab->setSelected(false);
    m_page->clearChildren();
    const PlayerStats& s = m_stats;
    addRow(*m_page, "Time played", {playTimeText(s.playSeconds)});
    addRow(*m_page, "Distance walked", {distanceText(s.walked)});
    addRow(*m_page, "Distance sprinted", {distanceText(s.sprinted)});
    addRow(*m_page, "Distance crouched", {distanceText(s.crouched)});
    addRow(*m_page, "Distance swum", {distanceText(s.swum)});
    addRow(*m_page, "Distance flown", {distanceText(s.flown)});
    addRow(*m_page, "Distance fallen", {distanceText(s.fallen)});
    addRow(*m_page, "Jumps", {std::to_string(s.jumps)});
    addRow(*m_page, "Long falls", {std::to_string(s.falls)});
    addRow(*m_page, "Blocks placed", {std::to_string(s.placedTotal())});
    addRow(*m_page, "Blocks broken", {std::to_string(s.brokenTotal())});
}

void StatisticsScreen::showBlocks() {
    using namespace vxe::ui;
    m_generalTab->setSelected(false);
    m_blocksTab->setSelected(true);
    m_page->clearChildren();
    struct Line {
        std::string name;
        vxe::u64 placed = 0;
        vxe::u64 broken = 0;
    };
    std::vector<Line> lines;
    const auto line = [&lines](const std::string& name) -> Line& {
        const auto found = std::ranges::find(lines, name, &Line::name);
        return found != lines.end() ? *found : lines.emplace_back(Line{.name = name});
    };
    for (const auto& [name, count] : m_stats.placed) {
        line(name).placed = count;
    }
    for (const auto& [name, count] : m_stats.broken) {
        line(name).broken = count;
    }
    if (lines.empty()) {
        m_page->add<Label>("No blocks placed or broken yet.", TextRole::Muted);
        return;
    }
    std::ranges::sort(lines, [](const Line& a, const Line& b) {
        return a.placed + a.broken != b.placed + b.broken ? a.placed + a.broken > b.placed + b.broken
                                                          : a.name < b.name;
    });
    addRow(*m_page, "Block", {"Placed", "Broken"}, TextRole::Muted);
    for (const Line& entry : lines) {
        const BlockDefinition* block = m_catalog.block(entry.name);
        addRow(*m_page, block != nullptr ? block->displayName : entry.name,
               {std::to_string(entry.placed), std::to_string(entry.broken)});
    }
}

} // namespace vr
