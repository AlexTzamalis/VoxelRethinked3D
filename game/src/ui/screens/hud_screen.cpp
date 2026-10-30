#include "hud_screen.hpp"

#include <vxe/ui/color.hpp>
#include <vxe/ui/pixel_font.hpp>

#include <cmath>
#include <string_view>

namespace vr {
namespace {

using vxe::f32;

constexpr f32 kSlot = 22.0F;     // The size of ui/hotbar_slot.png.
constexpr f32 kSelected = 24.0F; // The size of ui/hotbar_selected.png.
constexpr f32 kCrosshair = 9.0F; // The size of ui/crosshair.png.
constexpr f32 kIcon = 16.0F;
constexpr f32 kBottomGap = 3.0F;
constexpr f32 kNameSeconds = 2.0F;

} // namespace

void HudWidget::sync(bool shown, f32 opacity, f32 deltaSeconds) {
    bool changed = shown != m_shown || opacity != m_opacity;
    m_shown = shown;
    m_opacity = opacity;
    if (m_hotbar.selected != m_drawn.selected || m_hotbar.slots != m_drawn.slots) {
        const bool otherBlock = m_hotbar.current() != m_drawn.current();
        m_drawn = m_hotbar;
        changed = true;
        if (otherBlock) {
            m_name.clear();
            for (const BlockDefinition& block : m_catalog.blocks()) {
                if (m_hotbar.current() >= block.firstState &&
                    m_hotbar.current() < block.firstState + block.stateCount) {
                    m_name = block.displayName;
                }
            }
            m_nameSeconds = kNameSeconds;
        }
    }
    if (m_nameSeconds > 0.0F) {
        m_nameSeconds -= deltaSeconds;
        changed = changed || m_nameSeconds <= 0.0F;
    }
    if (changed) {
        invalidateVisual();
    }
}

void HudWidget::draw(vxe::ui::DrawContext& context) const {
    if (!m_shown) {
        return;
    }
    const vxe::ui::UiRect& box = rect();
    const auto alpha = static_cast<vxe::u8>(std::lround(m_opacity * 255.0F));
    const vxe::u32 tint = vxe::ui::rgba(255, 255, 255, alpha);
    const f32 centerX = std::floor(box.width / 2.0F);

    context.image({box.x + centerX - std::floor(kCrosshair / 2.0F),
                   box.y + std::floor((box.height - kCrosshair) / 2.0F), kCrosshair, kCrosshair},
                  "crosshair");

    const f32 barWidth = kSlot * static_cast<f32>(m_drawn.slots.size());
    const f32 barX = box.x + std::floor((box.width - barWidth) / 2.0F);
    const f32 barY = box.y + box.height - kSlot - kBottomGap;
    for (vxe::usize slot = 0; slot < m_drawn.slots.size(); ++slot) {
        const f32 x = barX + (kSlot * static_cast<f32>(slot));
        context.image({x, barY, kSlot, kSlot}, "hotbar_slot", tint);
    }
    for (vxe::usize slot = 0; slot < m_drawn.slots.size(); ++slot) {
        const std::string_view icon = m_icons.iconOf(m_drawn.slots.at(slot));
        if (!icon.empty()) {
            const f32 x = barX + (kSlot * static_cast<f32>(slot)) + ((kSlot - kIcon) / 2.0F);
            context.image({x, barY + ((kSlot - kIcon) / 2.0F), kIcon, kIcon}, icon, tint);
        }
    }
    const f32 selectedX = barX + (kSlot * static_cast<f32>(m_drawn.selected)) - 1.0F;
    context.image({selectedX, barY - 1.0F, kSelected, kSelected}, "hotbar_selected", tint);

    if (m_nameSeconds > 0.0F && !m_name.empty() && context.font() != nullptr) {
        const f32 width = std::ceil(context.font()->measure(m_name).width);
        context.text(m_name, box.x + std::floor((box.width - width) / 2.0F), barY - 12.0F,
                     context.theme().colors.text, true);
    }
}

void HudScreen::build(vxe::ui::Widget& root) {
    m_hud = &root.add<HudWidget>(m_hotbar, m_icons, m_catalog);
    m_hud->style().grow = 1.0F;
}

} // namespace vr
