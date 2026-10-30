#include "../test_helpers.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/rhi_null/null_device.hpp>
#include <vxe/ui/dialog.hpp>
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

// A UI at scale 1 (UI pixels are window pixels) with a font where every
// glyph is 5 pixels wide (advance 6).
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
    }

    // Without a pointer by default, so nothing gets hovered by accident.
    void frame(UiInput input = {.pointer = {.available = false}}) const {
        context->update(640, 480, input, *renderer);
        renderer->begin(640, 480);
        context->draw(*renderer);
    }
    void keys(NavigationInput navigation) const {
        frame({.pointer = {.available = false}, .navigation = navigation});
    }
    void type(TextEditInput text) const { frame({.pointer = {.available = false}, .text = text}); }
    void pointer(f32 x, f32 y, bool down = false, f32 seconds = 0.0F) const {
        frame({.pointer = {.x = x, .y = y, .primaryDown = down}, .deltaSeconds = seconds});
    }
    void click(const Widget& widget) const {
        const UiRect& box = widget.rect();
        pointer(box.x + 2.0F, box.y + box.height / 2.0F);
        pointer(box.x + 2.0F, box.y + box.height / 2.0F, true);
        pointer(box.x + 2.0F, box.y + box.height / 2.0F);
    }
};

} // namespace

TEST_CASE("Tab walks the focusable widgets, the arrows move to the nearest one, Enter clicks") {
    const Fixture ui;
    Widget& root = ui.context->root();
    // A 2 x 2 grid of buttons and a disabled one that focus skips.
    std::vector<int> clicks(4, 0);
    std::vector<Button*> buttons;
    for (int row = 0; row < 2; ++row) {
        auto& line = root.add<Widget>();
        line.style().direction = Direction::Row;
        line.style().alignSelf = Align::Start;
        for (int column = 0; column < 2; ++column) {
            const auto index = (static_cast<usize>(row) * 2) + static_cast<usize>(column);
            buttons.push_back(&line.add<Button>("B", [&clicks, index] { ++clicks[index]; }));
        }
    }
    root.add<Button>("Off").setEnabled(false);
    ui.frame();
    CHECK(ui.context->focused() == nullptr);

    ui.keys({.next = true});
    CHECK(ui.context->focused() == buttons[0]);
    CHECK(buttons[0]->focused());
    ui.keys({.next = true});
    ui.keys({.next = true});
    ui.keys({.next = true});
    CHECK(ui.context->focused() == buttons[3]);
    ui.keys({.next = true}); // Wraps around, past the disabled button.
    CHECK(ui.context->focused() == buttons[0]);
    ui.keys({.previous = true});
    CHECK(ui.context->focused() == buttons[3]);

    ui.keys({.up = true});
    CHECK(ui.context->focused() == buttons[1]);
    ui.keys({.left = true});
    CHECK(ui.context->focused() == buttons[0]);
    ui.keys({.left = true}); // Nothing further left: stays.
    CHECK(ui.context->focused() == buttons[0]);
    ui.keys({.down = true});
    CHECK(ui.context->focused() == buttons[2]);

    ui.keys({.activate = true});
    CHECK(clicks == std::vector<int>{0, 0, 1, 0});
}

TEST_CASE("Hover moves the focus, a text field keeps it until something else is clicked") {
    const Fixture ui;
    Widget& root = ui.context->root();
    root.style().align = Align::Start;
    auto& first = root.add<Button>("One");
    auto& second = root.add<Button>("Two");
    auto& field = root.add<TextField>();
    ui.frame();

    ui.pointer(first.rect().x + 2.0F, first.rect().y + 2.0F);
    CHECK(ui.context->focused() == &first);
    ui.pointer(600.0F, 400.0F); // Off every widget: focus from hovering goes away.
    CHECK(ui.context->focused() == nullptr);

    ui.click(field);
    CHECK(ui.context->focused() == &field);
    CHECK(ui.context->wantsTextInput());
    ui.pointer(second.rect().x + 2.0F, second.rect().y + 2.0F);
    CHECK(ui.context->focused() == &field); // Hovering does not steal a text field's focus.
    ui.click(second);
    CHECK(ui.context->focused() == &second);
    CHECK_FALSE(ui.context->wantsTextInput());
    ui.pointer(600.0F, 400.0F);
    ui.pointer(600.0F, 400.0F, true); // Clicking nothing clears the focus.
    CHECK(ui.context->focused() == nullptr);
}

TEST_CASE("Text fields edit UTF-8 text at the caret") {
    const Fixture ui;
    Widget& root = ui.context->root();
    root.style().align = Align::Start;
    auto& field = root.add<TextField>();
    std::vector<std::string> changes;
    std::string submitted;
    field.setOnChange([&changes](const std::string& text) { changes.push_back(text); });
    field.setOnSubmit([&submitted](const std::string& text) { submitted = text; });
    field.setMaxLength(6);
    ui.frame();
    ui.click(field);

    ui.type({.typed = "ab\xC3\xA9"}); // "abé"
    CHECK(field.text() == "ab\xC3\xA9");
    CHECK(field.caret() == 4);
    ui.type({.caretLeft = true});
    CHECK(field.caret() == 2); // Before the two byte é, never inside it.
    ui.type({.typed = "\tX"}); // Control characters are dropped.
    CHECK(field.text() == "abX\xC3\xA9");
    ui.type({.eraseNext = true});
    CHECK(field.text() == "abX");
    ui.type({.erase = true});
    CHECK(field.text() == "ab");
    ui.type({.home = true});
    ui.type({.typed = "12345678"}); // Only up to 6 code points.
    CHECK(field.text() == "1234ab");
    ui.type({.end = true});
    CHECK(field.caret() == field.text().size());
    CHECK(changes.size() == 5);
    CHECK(changes.back() == "1234ab");

    // Left and Right move the caret, not the focus.
    ui.keys({.left = true});
    CHECK(ui.context->focused() == &field);
    ui.keys({.activate = true});
    CHECK(submitted == "1234ab");

    // A filter: digits only.
    field.setText("");
    field.setFilter([](char32_t c) { return c >= U'0' && c <= U'9'; });
    ui.type({.typed = "4a2-"});
    CHECK(field.text() == "42");

    // A click places the caret at the nearest character boundary: each
    // glyph advances 6 pixels after the 4 pixel inset.
    field.setText("abcdef");
    ui.frame();
    const UiRect box = field.rect();
    ui.pointer(box.x + 4.0F + 13.0F, box.y + 2.0F, true);
    ui.pointer(box.x + 4.0F + 13.0F, box.y + 2.0F);
    CHECK(field.caret() == 2);
}

TEST_CASE("Back closes the top dialog or goes to the screen") {
    const Fixture ui;
    Widget& root = ui.context->root();
    root.style().align = Align::Start;
    auto& open = root.add<Button>("Open");
    std::string result;
    ui.frame();

    ui.keys({.back = true});
    CHECK(ui.context->takeBack());
    CHECK_FALSE(ui.context->takeBack()); // Once.

    ui.keys({.next = true});
    CHECK(ui.context->focused() == &open);
    showDialog(*ui.context,
               {.title = "Delete world?",
                .message = "It will be gone for good.",
                .buttons = {{.text = "Delete", .onClick = [&result] { result = "deleted"; }},
                            {.text = "Cancel", .onClick = [&result] { result = "kept"; }, .cancel = true}}});
    ui.frame();
    CHECK(ui.context->layerCount() == 1);
    // The cancel button starts focused; the screen below takes no pointer.
    REQUIRE(ui.context->focused() != nullptr);
    CHECK(dynamic_cast<Button*>(ui.context->focused())->text() == "Cancel");
    ui.pointer(open.rect().x + 2.0F, open.rect().y + 2.0F);
    CHECK_FALSE(open.hovered());
    CHECK(ui.context->wantsPointer());

    ui.keys({.back = true}); // Back means cancel.
    ui.frame();
    CHECK(result == "kept");
    CHECK(ui.context->layerCount() == 0);
    CHECK_FALSE(ui.context->takeBack());   // The dialog took it.
    CHECK(ui.context->focused() == &open); // Focus comes back.

    // Enter on the other button confirms.
    showDialog(*ui.context, {.title = "Delete world?",
                             .buttons = {{.text = "Delete", .onClick = [&result] { result = "deleted"; }},
                                         {.text = "Cancel", .cancel = true}}});
    ui.frame();
    ui.keys({.previous = true});
    ui.keys({.activate = true});
    ui.frame();
    CHECK(result == "deleted");
    CHECK(ui.context->layerCount() == 0);
}

TEST_CASE("Tooltips show after the pointer rests, also on disabled buttons") {
    const Fixture ui;
    Widget& root = ui.context->root();
    root.style().align = Align::Start;
    auto& off = root.add<Button>("Multiplayer");
    off.setEnabled(false);
    off.setTooltip("Coming later");
    ui.frame();
    const f32 x = off.rect().x + 4.0F;
    const f32 y = off.rect().y + 4.0F;
    ui.pointer(x, y, false, 0.1F);
    CHECK(ui.context->visibleTooltip().empty());
    ui.pointer(x, y, false, 0.3F);
    CHECK(ui.context->visibleTooltip().empty());
    const u64 before = ui.context->rebuildCount();
    ui.pointer(x, y, false, 0.3F);
    CHECK(ui.context->visibleTooltip() == "Coming later");
    CHECK(ui.context->rebuildCount() == before + 1); // Shown once, then kept.
    ui.pointer(x + 1.0F, y, false, 0.1F);
    CHECK(ui.context->visibleTooltip() == "Coming later"); // Small moves keep it.
    ui.pointer(600.0F, 400.0F, false, 0.1F);
    CHECK(ui.context->visibleTooltip().empty());
}
