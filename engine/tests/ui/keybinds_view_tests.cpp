#include "../test_helpers.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/input/keybinds.hpp>
#include <vxe/rhi_null/null_device.hpp>
#include <vxe/ui/keybinds_view.hpp>
#include <vxe/ui/ui_context.hpp>
#include <vxe/ui/ui_renderer.hpp>
#include <vxe/ui/widgets.hpp>

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

using namespace vxe;
using namespace vxe::ui;
using input::Binding;
namespace scancode = input::scancode;

namespace {

constexpr u16 kKeyJ = 13;
constexpr u16 kKeyK = 14;

// A UI at scale 1 with three actions: two in the same context, which can
// conflict, and one in a separate context, which cannot.
struct Fixture {
    std::unique_ptr<rhi::Device> device = rhi::createNullDevice();
    std::unique_ptr<UiRenderer> renderer;
    config::SettingsRegistry settings;
    input::InputSystem input;
    std::unique_ptr<UiContext> context;
    input::InputContextId gameplay = input.registerContext({.name = "Gameplay"});
    input::InputContextId menu = input.registerContext({.name = "Menu"});
    input::ActionId jump =
        addAction("test.jump", "Jump", "Movement", gameplay, Binding::key(scancode::kSpace));
    input::ActionId crouch =
        addAction("test.crouch", "Crouch", "Movement", gameplay, Binding::key(scancode::kE));
    input::ActionId open = addAction("test.open", "Open", "Interface", menu, Binding::key(scancode::kSpace));

    input::ActionId addAction(std::string_view id, std::string_view name, std::string_view category,
                              input::InputContextId owner, Binding binding) {
        return input.registerAction({.id = id,
                                     .displayName = name,
                                     .category = category,
                                     .context = owner,
                                     .defaults = {binding, Binding{}, Binding{}}});
    }

    Fixture() {
        auto created = UiRenderer::create(
            {.device = device.get(), .shaders = rhi::folderShaderReader(VXE_ENGINE_SHADER_DIR)});
        REQUIRE(created.hasValue());
        renderer = std::move(created).value();
        const std::vector<u32> pixels(usize{128} * 128, 0xFFFFFFFFU);
        REQUIRE(renderer->setFont(pixels, 128, 128).hasValue());
        context = UiContext::create({.settings = &settings});
        REQUIRE(settings.set("Interface.UiScale", config::SettingValue{i32{1}}).hasValue());
    }

    void frame(UiInput uiInput = {.pointer = {.available = false}}) const {
        context->update(640, 480, uiInput, *renderer);
        renderer->begin(640, 480);
        context->draw(*renderer);
    }
    void keys(NavigationInput navigation) const {
        frame({.pointer = {.available = false}, .navigation = navigation});
    }
    void press(u16 key) {
        const std::vector<platform::PlatformEvent> down{platform::KeyEvent{.scancode = key, .down = true}};
        input.update(down);
        frame();
        const std::vector<platform::PlatformEvent> up{platform::KeyEvent{.scancode = key, .down = false}};
        input.update(up);
    }
    // Starts capturing a slot by keyboard, like Enter on a focused field.
    void capture(KeybindField& field) const {
        context->setFocus(&field);
        keys({.activate = true});
        REQUIRE(field.capturing());
    }
    // Moves the dialog's focus left of Cancel and presses the button there.
    void answer(usize stepsLeftOfCancel) const {
        frame();
        for (usize step = 0; step < stepsLeftOfCancel; ++step) {
            keys({.previous = true});
        }
        keys({.activate = true});
        frame();
    }
};

template <typename T>
// NOLINTNEXTLINE(misc-no-recursion): bounded by the depth of the widget tree.
void collect(Widget& widget, std::vector<T*>& out) {
    if (auto* found = dynamic_cast<T*>(&widget)) {
        out.push_back(found);
    }
    for (const auto& child : widget.children()) {
        collect(*child, out);
    }
}

// The fields of a view: two per row, in the order of the rows (Movement:
// Jump, Crouch; Interface: Open).
std::vector<KeybindField*> fieldsOf(Widget& view) {
    std::vector<KeybindField*> fields;
    collect(view, fields);
    return fields;
}

Button& buttonNamed(Widget& view, std::string_view name, usize skip = 0) {
    std::vector<Button*> buttons;
    collect(view, buttons);
    for (Button* button : buttons) {
        if (button->text() == name && skip-- == 0) {
            return *button;
        }
    }
    FAIL("No such button");
    return *buttons.front();
}

} // namespace

TEST_CASE("Binding labels read like the keyboard") {
    CHECK(input::bindingLabel(Binding{}).empty());
    CHECK(input::bindingLabel(Binding::mouse(platform::MouseButton::Left)) == "Mouse Left");
    CHECK_FALSE(input::bindingLabel(Binding::key(scancode::kSpace)).empty());
    CHECK(
        input::bindingLabel(Binding::key(scancode::kSpace, platform::Modifiers::Ctrl)).starts_with("Ctrl+"));
}

TEST_CASE("A slot captures a key, Escape cancels and Backspace clears") {
    Fixture ui;
    auto& view = ui.context->root().add<KeybindsView>(ui.input, *ui.context);
    ui.frame();
    CHECK(view.rowCount() == 3);
    const std::vector<KeybindField*> fields = fieldsOf(view);
    REQUIRE(fields.size() == 6);
    CHECK(fields[1]->label().empty()); // Jump's free second slot.

    ui.capture(*fields[1]);
    ui.press(kKeyJ);
    CHECK_FALSE(fields[1]->capturing());
    CHECK(ui.input.action(ui.jump).bindings[1] == Binding::key(kKeyJ));
    CHECK_FALSE(fields[1]->label().empty());

    ui.capture(*fields[1]);
    ui.press(scancode::kEscape);
    CHECK(ui.input.action(ui.jump).bindings[1] == Binding::key(kKeyJ)); // Kept.
    CHECK_FALSE(ui.context->takeBack()); // The capture took Escape, not the menu.

    ui.capture(*fields[1]);
    ui.press(scancode::kBackspace);
    CHECK(ui.input.action(ui.jump).bindings[1].isNone());
}

TEST_CASE("A key another action uses asks what to do") {
    Fixture ui;
    auto& view = ui.context->root().add<KeybindsView>(ui.input, *ui.context);
    ui.frame();
    const std::vector<KeybindField*> fields = fieldsOf(view);
    KeybindField& jumpSlot = *fields[0];
    const Binding space = Binding::key(scancode::kSpace);
    const Binding e = Binding::key(scancode::kE);

    SUBCASE("Cancel changes nothing, and Escape means cancel") {
        ui.capture(jumpSlot);
        ui.press(scancode::kE);
        CHECK(ui.context->layerCount() == 1);
        ui.keys({.back = true});
        ui.frame();
        CHECK(ui.context->layerCount() == 0);
        CHECK(ui.input.action(ui.jump).bindings[0] == space);
        CHECK(ui.input.action(ui.crouch).bindings[0] == e);
    }
    SUBCASE("Swap trades the two keys") {
        ui.capture(jumpSlot);
        ui.press(scancode::kE);
        ui.answer(3); // Swap, Unbind other, Keep both, Cancel.
        CHECK(ui.input.action(ui.jump).bindings[0] == e);
        CHECK(ui.input.action(ui.crouch).bindings[0] == space);
    }
    SUBCASE("Unbind other frees the key") {
        ui.capture(jumpSlot);
        ui.press(scancode::kE);
        ui.answer(2);
        CHECK(ui.input.action(ui.jump).bindings[0] == e);
        CHECK(ui.input.action(ui.crouch).bindings[0].isNone());
    }
    SUBCASE("Keep both leaves a warning on both rows") {
        ui.capture(jumpSlot);
        ui.press(scancode::kE);
        ui.answer(1);
        CHECK(ui.input.action(ui.jump).bindings[0] == e);
        CHECK(ui.input.action(ui.crouch).bindings[0] == e);
        std::vector<Label*> labels;
        collect(view, labels);
        const auto warnings =
            std::ranges::count_if(labels, [](const Label* label) { return label->text() == "!"; });
        CHECK(warnings == 2);
    }
    SUBCASE("The same key in another context is no conflict") {
        ui.capture(*fields[4]); // Open already uses Space, in the Menu context.
        ui.press(scancode::kE);
        CHECK(ui.context->layerCount() == 0);
        CHECK(ui.input.action(ui.open).bindings[0] == e);
    }
}

TEST_CASE("Reset works per action, per group and for everything") {
    Fixture ui;
    auto& view = ui.context->root().add<KeybindsView>(ui.input, *ui.context);
    ui.frame();
    const Binding space = Binding::key(scancode::kSpace);
    ui.input.setBinding(ui.jump, 0, Binding::key(kKeyJ));
    ui.input.setBinding(ui.crouch, 0, Binding::key(kKeyK));
    ui.input.setBinding(ui.open, 0, Binding::key(kKeyK));
    view.refresh();

    Button& resetJump = buttonNamed(view, "Reset");
    CHECK(resetJump.enabled());
    resetJump.activate();
    CHECK(ui.input.action(ui.jump).bindings[0] == space);
    CHECK_FALSE(resetJump.enabled()); // Back at the default.

    buttonNamed(view, "Reset group").activate(); // Movement.
    CHECK(ui.input.action(ui.crouch).bindings[0] == Binding::key(scancode::kE));
    CHECK(ui.input.action(ui.open).bindings[0] == Binding::key(kKeyK));

    buttonNamed(view, "Reset all").activate();
    ui.frame();
    CHECK(ui.context->layerCount() == 1); // It asks first.
    ui.answer(1);
    CHECK(ui.input.action(ui.open).bindings[0] == space);
}

TEST_CASE("Searching filters the list and the search box keeps the focus") {
    Fixture ui;
    auto& view = ui.context->root().add<KeybindsView>(ui.input, *ui.context);
    ui.frame();
    std::vector<TextField*> fields;
    collect(view, fields);
    REQUIRE(fields.size() == 1);
    TextField& search = *fields.front();
    ui.context->setFocus(&search);

    ui.frame({.pointer = {.available = false}, .text = {.typed = "cro"}});
    CHECK(view.rowCount() == 1);
    ui.frame();
    CHECK(ui.context->focused() == &search); // Rebuilding the rows did not take it.
    ui.frame({.pointer = {.available = false}, .text = {.typed = "x"}});
    CHECK(search.text() == "crox");
    CHECK(view.rowCount() == 0);
    view.setFilter("interface"); // By category.
    CHECK(view.rowCount() == 1);
}

TEST_CASE("Leaving during a capture stops it") {
    Fixture ui;
    Widget& root = ui.context->root();
    auto& view = root.add<KeybindsView>(ui.input, *ui.context);
    ui.frame();
    ui.capture(*fieldsOf(view).front());
    CHECK(ui.input.isCapturing());
    root.clearChildren();
    CHECK_FALSE(ui.input.isCapturing());
    ui.frame();
    CHECK(ui.context->focused() == nullptr);
}

TEST_CASE("Widgets that still exist keep the pointer and focus when the tree changes") {
    const Fixture ui;
    Widget& root = ui.context->root();
    root.style().align = Align::Start;
    auto& kept = root.add<Button>("Kept");
    auto& list = root.add<Widget>();
    auto& gone = list.add<Button>("Gone");
    ui.frame();

    ui.context->setFocus(&kept);
    list.clearChildren();
    ui.frame();
    CHECK(ui.context->focused() == &kept);

    auto& next = list.add<Button>("Next");
    ui.frame();
    ui.context->setFocus(&next);
    list.clearChildren();
    ui.frame();
    CHECK(ui.context->focused() == nullptr); // Removed: nothing stale remains.

    ui.context->setFocus(&kept);
    kept.setVisible(false);
    ui.frame();
    CHECK(ui.context->focused() == nullptr);
    CHECK_FALSE(kept.focused()); // Hidden, alive, and told about it.
    static_cast<void>(gone);
}
