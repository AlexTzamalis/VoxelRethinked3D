#include "ui/screens/loading_screen.hpp"

#include <vxe/ui/controls.hpp>
#include <vxe/ui/widgets.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <utility>

namespace vr {
namespace {

using vxe::f32;

constexpr f32 kTile = 32.0F; // UI pixels per background tile: twice a 16 pixel texture.
constexpr f32 kBarWidth = 200.0F;
constexpr f32 kBarHeight = 4.0F;
constexpr f32 kEaseRate = 4.0F; // Per second towards the estimate.

// Short enough for one line at the smallest UI (320 pixels across).
constexpr std::array<std::string_view, 10> kTips = {
    "Tip: double tap Space to fly.",
    "Tip: hold C to zoom, and scroll to zoom more.",
    "Tip: press E to see every block.",
    "Tip: middle click picks the block you aim at.",
    "Tip: press F3 for the performance overlay.",
    "Tip: F11 switches between window and fullscreen.",
    "Tip: walking and flying speeds are in Settings.",
    "Tip: rivers start as streams high in the hills.",
    "Tip: snow piles up in layers, eight to a block.",
    "Tip: far terrain quality is set per world.",
};

} // namespace

std::string_view randomLoadingTip() {
    std::random_device device;
    return kTips.at(device() % kTips.size());
}

void LoadingWidget::sync(f32 fraction, std::string_view stage, f32 deltaSeconds) {
    const f32 target = std::clamp(fraction, 0.0F, 1.0F);
    const f32 before = m_shown;
    if (target > m_shown) {
        m_shown += (target - m_shown) * std::min(deltaSeconds * kEaseRate, 1.0F);
    }
    const bool moved = std::abs(m_shown - before) * kBarWidth >= 0.25F;
    if (moved || stage != m_stage) {
        m_stage = stage;
        invalidateVisual();
    }
}

void LoadingWidget::draw(vxe::ui::DrawContext& context) const {
    const vxe::ui::UiRect& box = rect();
    // A dark tiled floor, as menus in Minecraft are, so the world building
    // behind stays hidden.
    const vxe::u32 shade = vxe::ui::rgba(64, 64, 64);
    bool tiled = true;
    for (f32 y = box.y; y < box.y + box.height && tiled; y += kTile) {
        for (f32 x = box.x; x < box.x + box.width && tiled; x += kTile) {
            tiled = context.image({x, y, kTile, kTile}, "loading_background", shade);
        }
    }
    if (!tiled) {
        context.fillRect(box, vxe::ui::rgba(30, 24, 20));
    }
    if (context.font() == nullptr) {
        return;
    }
    const auto centered = [&](std::string_view text, f32 y, vxe::u32 scale) {
        const f32 width = std::ceil(context.font()->measure(text).width) * static_cast<f32>(scale);
        context.text(text, box.x + std::floor((box.width - width) / 2.0F), y, context.theme().colors.text,
                     true, scale);
    };
    const f32 middle = box.y + std::floor(box.height / 2.0F);
    centered("Loading world", middle - 34.0F, 2);
    centered(m_stage, middle - 12.0F, 1);
    const f32 barX = box.x + std::floor((box.width - kBarWidth) / 2.0F);
    const f32 barY = middle + 4.0F;
    context.fillRect({barX - 1.0F, barY - 1.0F, kBarWidth + 2.0F, kBarHeight + 2.0F},
                     vxe::ui::rgba(0, 0, 0, 200));
    context.fillRect({barX, barY, std::floor(kBarWidth * m_shown), kBarHeight}, vxe::ui::rgba(128, 255, 128));
    const f32 tipWidth = std::ceil(context.font()->measure(m_tip).width);
    context.text(m_tip, box.x + std::floor((box.width - tipWidth) / 2.0F), barY + 16.0F,
                 context.theme().colors.textMuted, true, 1);
}

void LoadingScreen::build(vxe::ui::Widget& root) {
    using namespace vxe::ui;
    m_loading = &root.add<LoadingWidget>(std::string(randomLoadingTip()));
    m_loading->style().grow = 1.0F;
    if (!m_cancel) {
        return;
    }
    // The button sits low on the dark floor the widget draws behind it.
    m_loading->style().justify = Justify::End;
    m_loading->style().align = Align::Center;
    m_loading->style().padding = Edges::all(24.0F);
    auto& cancel = m_loading->add<Button>("Cancel", [this] { m_cancel(); });
    cancel.setTooltip("Back to the title. Nothing is lost.");
}

} // namespace vr
