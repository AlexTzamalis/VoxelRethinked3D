// UI benchmarks (E12.2): laying out and drawing a menu sized widget tree.
// Layout runs only when the tree, window or scale changed; drawing runs
// every frame the UI is shown.

#include "benchmarks.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/rhi_null/null_device.hpp>
#include <vxe/ui/settings_view.hpp>
#include <vxe/ui/ui_renderer.hpp>
#include <vxe/ui/widgets.hpp>

#include <nanobench.h>

#include <format>
#include <stdexcept>
#include <string>
#include <vector>

namespace vxe::bench {
namespace {

// A font with every glyph 5 pixels wide, enough for realistic text sizes.
ui::PixelFont blockFont() {
    std::vector<u32> pixels(usize{128} * 128, 0U);
    for (u32 code = 0; code < 256; ++code) {
        for (u32 y = 0; y < 8; ++y) {
            for (u32 x = 0; x < 5; ++x) {
                pixels[(usize{(code / 16) * 8 + y} * 128) + usize{(code % 16) * 8 + x}] = 0xFFFFFFFFU;
            }
        }
    }
    auto font = ui::PixelFont::fromSheet(pixels, 128, 128);
    if (!font) {
        throw std::runtime_error("font");
    }
    return std::move(font).value();
}

// A settings like screen: a panel with 40 rows of a label and two buttons,
// plus a wrapped description: 162 widgets.
void buildMenu(ui::Widget& root) {
    root.style().justify = ui::Justify::Center;
    root.style().align = ui::Align::Center;
    auto& panel = root.add<ui::Panel>();
    panel.style().width = 300.0F;
    auto& description = panel.add<ui::Label>(
        "Every setting of this category, generated from the settings registry, with a reset button each.");
    description.setWrap(true);
    for (int i = 0; i < 40; ++i) {
        auto& row = panel.add<ui::Widget>();
        row.style().direction = ui::Direction::Row;
        row.style().gap = 4.0F;
        auto& label = row.add<ui::Label>(std::format("Setting number {}", i));
        label.style().grow = 1.0F;
        row.add<ui::Button>("Change");
        row.add<ui::Button>("Reset");
    }
}

} // namespace

void runUiBenchmarks() {
    const ui::PixelFont font = blockFont();
    const ui::Theme theme;
    ui::Widget root;
    buildMenu(root);
    const ui::UiEnvironment environment{.theme = theme, .font = &font};

    ankerl::nanobench::Bench bench;
    bench.title("UI (162 widgets)").unit("tree").warmup(10).minEpochIterations(50);
    bench.run("layout", [&] {
        root.arrange(environment, {0.0F, 0.0F, 480.0F, 1200.0F});
        ankerl::nanobench::doNotOptimizeAway(root.rect());
    });

    const auto device = rhi::createNullDevice();
    auto renderer = ui::UiRenderer::create(
        {.device = device.get(), .shaders = rhi::folderShaderReader(VXE_ENGINE_SHADER_DIR)});
    if (!renderer) {
        throw std::runtime_error("UI renderer");
    }
    std::vector<u32> pixels(usize{128} * 128, 0xFFFFFFFFU);
    if (!renderer.value()->setFont(pixels, 128, 128)) {
        throw std::runtime_error("UI font");
    }
    bench.run("draw into quads (scale 3)", [&] {
        renderer.value()->begin(1440, 3600);
        ui::DrawContext context(renderer.value()->frame(), theme, 3);
        root.drawTree(context);
        ankerl::nanobench::doNotOptimizeAway(renderer.value()->quadCount());
    });
    // A still screen: UiContext keeps the quads and only appends them.
    ui::QuadBatch kept;
    kept.reset(&renderer.value()->resources());
    ui::DrawContext keptContext(kept, theme, 3);
    root.drawTree(keptContext);
    bench.run("append kept quads (still screen)", [&] {
        renderer.value()->begin(1440, 3600);
        renderer.value()->frame().append(kept);
        ankerl::nanobench::doNotOptimizeAway(renderer.value()->quadCount());
    });

    // The settings screen syncs its controls with the registry every frame.
    config::SettingsRegistry settings;
    std::vector<config::Setting<i32>> handles;
    handles.reserve(60);
    std::vector<std::string> ids;
    ids.reserve(60);
    for (int i = 0; i < 60; ++i) {
        ids.push_back(std::format("Graphics.Group{}.Setting{}", i / 10, i));
        handles.push_back(settings.registerInt(
            {{.id = ids.back(), .displayName = "A setting", .description = "What it does."}, 5, 0, 10, 1}));
    }
    ui::SettingsView view(settings);
    view.showGroup("Graphics");
    bench.title("SettingsView (60 settings)").unit("refresh");
    bench.run("refresh", [&] {
        view.refresh();
        ankerl::nanobench::doNotOptimizeAway(view.rowCount());
    });
}

} // namespace vxe::bench
