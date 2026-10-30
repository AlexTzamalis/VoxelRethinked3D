#include "../test_helpers.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/rhi_null/null_device.hpp>
#include <vxe/ui/controls.hpp>
#include <vxe/ui/settings_view.hpp>
#include <vxe/ui/ui_context.hpp>
#include <vxe/ui/ui_renderer.hpp>
#include <vxe/ui/widgets.hpp>

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

using namespace vxe;
using namespace vxe::ui;

namespace {

// A UI at scale 1 with a font where every glyph is 5 pixels wide (advance 6).
struct Fixture {
    std::unique_ptr<rhi::Device> device = rhi::createNullDevice();
    std::unique_ptr<UiRenderer> renderer;
    config::SettingsRegistry settings;
    std::unique_ptr<UiContext> context;

    Fixture() {
        auto created = UiRenderer::create(
            {.device = device.get(), .shaders = rhi::folderShaderReader(VXE_ENGINE_SHADER_DIR)});
        REQUIRE(created.hasValue());
        renderer = std::move(created).value();
        std::vector<u32> pixels(usize{128} * 128, 0U);
        for (u32 code = 0; code < 256; ++code) {
            for (u32 y = 0; y < 8; ++y) {
                for (u32 x = 0; x < 5; ++x) {
                    pixels[(usize{(code / 16) * 8 + y} * 128) + usize{(code % 16) * 8 + x}] = 0xFFFFFFFFU;
                }
            }
        }
        REQUIRE(renderer->setFont(pixels, 128, 128).hasValue());
        context = UiContext::create({.settings = &settings});
        REQUIRE(settings.set("Interface.UiScale", config::SettingValue{i32{1}}).hasValue());
        context->root().style().align = Align::Start;
    }
    void frame(UiInput input = {.pointer = {.available = false}}) const {
        context->update(640, 480, input, *renderer);
        renderer->begin(640, 480);
        context->draw(*renderer);
    }
    void keys(NavigationInput navigation) const {
        frame({.pointer = {.available = false}, .navigation = navigation});
    }
    void pointer(f32 x, f32 y, bool down = false, f32 wheel = 0.0F) const {
        frame({.pointer = {.x = x, .y = y, .primaryDown = down, .wheel = wheel}});
    }
    void click(const Widget& widget, f32 dx = 2.0F) const {
        const UiRect& box = widget.rect();
        pointer(box.x + dx, box.y + box.height / 2.0F);
        pointer(box.x + dx, box.y + box.height / 2.0F, true);
        pointer(box.x + dx, box.y + box.height / 2.0F);
    }
};

} // namespace

TEST_CASE("Toggles flip on click, Enter and the arrows") {
    const Fixture ui;
    std::vector<bool> changes;
    auto& toggle =
        ui.context->root().add<Toggle>(false, [&changes](bool value) { changes.push_back(value); });
    ui.frame();
    ui.click(toggle);
    CHECK(toggle.value());
    ui.keys({.next = true}); // Hovering already focused it; Tab keeps it (only focusable).
    ui.keys({.activate = true});
    CHECK_FALSE(toggle.value());
    ui.keys({.right = true});
    CHECK(toggle.value());
    ui.keys({.right = true}); // Already on.
    CHECK(changes == std::vector<bool>{true, false, true});
    toggle.setValue(false); // Syncing does not report a change.
    CHECK(changes.size() == 3);
}

TEST_CASE("Sliders snap to their step and follow the pointer and the arrows") {
    const Fixture ui;
    std::vector<f64> changes;
    auto& slider = ui.context->root().add<Slider>(0.0, 10.0, 2.0, 5.0,
                                                  [&changes](f64 value) { changes.push_back(value); });
    slider.style().width = 104.0F; // 100 usable pixels plus the 4 pixel knob.
    CHECK(slider.value() == 6.0);  // 5 snaps to the nearest step from the minimum.
    ui.frame();
    // A press at the far left goes to the minimum, a drag to the right end to the maximum.
    const UiRect box = slider.rect();
    ui.pointer(box.x + 1.0F, box.y + 2.0F);
    ui.pointer(box.x + 1.0F, box.y + 2.0F, true);
    CHECK(slider.value() == 0.0);
    CHECK(slider.dragging());
    ui.pointer(box.x + box.width + 50.0F, box.y + 2.0F, true);
    CHECK(slider.value() == 10.0);
    ui.pointer(box.x + box.width + 50.0F, box.y + 2.0F);
    CHECK_FALSE(slider.dragging());
    ui.keys({.next = true});
    ui.keys({.left = true});
    CHECK(slider.value() == 8.0);
    CHECK(changes == std::vector<f64>{0.0, 10.0, 8.0});

    // Decimal steps show their decimals.
    Slider fine(0.0, 1.0, 0.05, 0.25);
    fine.setValue(0.333);
    CHECK(fine.value() == doctest::Approx(0.35));
}

TEST_CASE("Cycle buttons go through their choices") {
    const Fixture ui;
    std::vector<usize> changes;
    auto& cycle = ui.context->root().add<CycleButton>(std::vector<std::string>{"Low", "Medium", "High"}, 1,
                                                      [&changes](usize index) { changes.push_back(index); });
    ui.frame();
    ui.click(cycle);
    CHECK(cycle.index() == 2);
    ui.keys({.right = true});
    CHECK(cycle.index() == 0); // Wraps around.
    ui.keys({.left = true});
    CHECK(cycle.index() == 2);
    cycle.setIndex(1);
    CHECK(changes == std::vector<usize>{2, 0, 2});
}

TEST_CASE("Long text ends in dots where it does not fit") {
    const Fixture ui;
    const PixelFont& font = *ui.renderer->font();
    CHECK(font.ellipsize("short", 100.0F) == "short");
    // Each glyph advances 6; "..." is 17 wide plus the gap before it.
    const std::string cut = font.ellipsize("Frame rate in the background", 60.0F);
    CHECK(cut.ends_with("..."));
    CHECK(font.measure(cut).width <= 60.0F);
    CHECK(cut == "Frame r...");
}

TEST_CASE("Scroll views clip their content, scroll with the wheel and follow the focus") {
    const Fixture ui;
    Widget& root = ui.context->root();
    root.style().align = Align::Stretch;
    auto& scroll = root.add<ScrollView>();
    scroll.style().height = 100.0F;
    auto& column = scroll.add<Widget>();
    std::vector<Button*> buttons;
    buttons.reserve(10);
    for (int i = 0; i < 10; ++i) {
        buttons.push_back(&column.add<Button>("Row")); // 20 high each: 200 in all.
    }
    ui.frame();
    CHECK(scroll.maxScroll() == 100.0F);
    CHECK(scroll.scrollOffset() == 0.0F);
    // Nothing is drawn outside the view (the buttons below are cut off).
    for (const UiQuad& quad : ui.renderer->frame().quads()) {
        CHECK(quad.rect[1] + quad.rect[3] <= 100.0F + 0.001F);
    }

    ui.pointer(10.0F, 50.0F, false, -1.0F); // One wheel line down.
    ui.frame();
    CHECK(scroll.scrollOffset() == ScrollView::kWheelStep);
    CHECK(buttons[0]->rect().y == -ScrollView::kWheelStep);
    ui.pointer(10.0F, 50.0F, false, 100.0F); // Far up: clamped.
    ui.frame();
    CHECK(scroll.scrollOffset() == 0.0F);

    // Tabbing to the last button scrolls it into view (the wheel test left
    // the hovered button focused; start from nothing).
    ui.frame({.pointer = {.available = false}});
    ui.context->setFocus(nullptr);
    for (int i = 0; i < 10; ++i) {
        ui.keys({.next = true});
    }
    ui.frame();
    CHECK(ui.context->focused() == buttons[9]);
    CHECK(scroll.scrollOffset() == 100.0F);
}

TEST_CASE("The settings view builds a row for every setting and keeps them in sync") {
    Fixture ui;
    config::SettingsRegistry& settings = ui.settings;
    const auto vsync = settings.registerBool(
        {{.id = "Video.Vsync", .displayName = "Vertical sync", .description = {}}, true});
    const auto distance = settings.registerInt(
        {{.id = "Graphics.LOD.Distance", .displayName = "Distance", .description = "How far terrain shows."},
         8,
         2,
         32,
         2});
    const auto name = settings.registerString({{.id = "Graphics.Textures.Packs",
                                                .displayName = "Packs",
                                                .description = {},
                                                .applyMode = config::ApplyMode::RequiresRestart},
                                               ""});
    const auto secret = settings.registerBool({{.id = "Debug.Secret",
                                                .displayName = {},
                                                .description = {},
                                                .visibility = config::Visibility::Developer},
                                               false});
    static_cast<void>(secret);

    auto& view = ui.context->root().add<SettingsView>(settings);
    view.style().width = 600.0F;
    view.style().height = 400.0F;
    // Ordered groups; Debug has only a developer setting and the hidden view skips it.
    CHECK(view.groups() == std::vector<std::string>{"Video", "Graphics", "Interface"});
    CHECK(view.group() == "Video");
    CHECK(view.rowCount() == 1);
    ui.frame();

    view.showGroup("Graphics");
    ui.frame();
    CHECK(view.rowCount() == 2);
    // The row's slider writes the setting; Reset brings back the default.
    const Slider* slider = nullptr;
    const Button* reset = nullptr;
    std::vector<Widget*> stack{&view};
    while (!stack.empty()) {
        Widget* widget = stack.back();
        stack.pop_back();
        if (auto* found = dynamic_cast<Slider*>(widget); found != nullptr) {
            slider = found;
        }
        if (auto* button = dynamic_cast<Button*>(widget);
            button != nullptr && button->text() == "Reset" && reset == nullptr) {
            reset = button;
        }
        for (const auto& child : widget->children()) {
            stack.push_back(child.get());
        }
    }
    REQUIRE(slider != nullptr);
    REQUIRE(reset != nullptr);
    CHECK(slider->value() == 8.0);
    ui.click(*slider, slider->rect().width - 1.0F);
    CHECK(distance.get() == 32);
    view.refresh();
    ui.frame();
    // Changes made elsewhere show up after refresh().
    REQUIRE(settings.set("Graphics.LOD.Distance", config::SettingValue{i32{10}}).hasValue());
    view.refresh();
    CHECK(slider->value() == 10.0);
    CHECK(name.get().empty());

    // Searching goes through every group.
    view.setFilter("sync");
    CHECK(view.rowCount() == 1);
    view.setFilter("terrain"); // In a description.
    CHECK(view.rowCount() == 1);
    view.setFilter("nothing like this");
    CHECK(view.rowCount() == 0);
    static_cast<void>(vsync);
}

TEST_CASE("Names from ids read as words") {
    CHECK(spacedName("FpsLimit") == "Fps Limit");
    CHECK(spacedName("LODQuality") == "LOD Quality");
    CHECK(spacedName("Video") == "Video");
}
