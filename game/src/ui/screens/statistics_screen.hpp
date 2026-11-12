#pragma once

#include "common/block_catalog.hpp"
#include "common/player_stats.hpp"

#include <vxe/ui/screen.hpp>
#include <vxe/ui/widgets.hpp>

#include <string>

namespace vr {

// "1 h 05 min", "12 min", "40 s".
[[nodiscard]] std::string playTimeText(vxe::f64 seconds);
// "850 m" below a kilometer, "12.4 km" above (a block is a meter).
[[nodiscard]] std::string distanceText(vxe::f64 blocks);

// Statistics (asked by Alex 2026-11-12), from the pause menu: what the player
// did in this world. General has time, distances, jumps and falls; Blocks has
// every block placed or broken with both counts, the most used first. Shows a
// copy taken when it opened.
class StatisticsScreen final : public vxe::ui::Screen {
public:
    StatisticsScreen(PlayerStats stats, const BlockCatalog& catalog)
        : m_stats(std::move(stats))
        , m_catalog(catalog) {}

    [[nodiscard]] vxe::ui::ScreenOptions options() const override { return {.backdrop = true}; }
    void build(vxe::ui::Widget& root) override;

private:
    void showGeneral();
    void showBlocks();

    PlayerStats m_stats;
    const BlockCatalog& m_catalog;
    vxe::ui::Widget* m_page = nullptr;
    vxe::ui::Button* m_generalTab = nullptr;
    vxe::ui::Button* m_blocksTab = nullptr;
};

} // namespace vr
