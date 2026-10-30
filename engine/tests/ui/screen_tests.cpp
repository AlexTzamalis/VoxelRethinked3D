#include "../test_helpers.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/rhi_null/null_device.hpp>
#include <vxe/ui/screen.hpp>
#include <vxe/ui/ui_context.hpp>
#include <vxe/ui/ui_renderer.hpp>
#include <vxe/ui/widgets.hpp>

#include <doctest/doctest.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace vxe;
using namespace vxe::ui;

namespace {

// A screen with one button that records what happens to it.
class TestScreen final : public Screen {
public:
    TestScreen(std::string name, std::vector<std::string>& log, ScreenOptions options = {})
        : m_name(std::move(name))
        , m_log(log)
        , m_options(options) {}

    [[nodiscard]] ScreenOptions options() const override { return m_options; }
    void build(Widget& root) override {
        root.style().align = Align::Start;
        button = &root.add<Button>(m_name, [this] {
            if (onClick) {
                onClick();
            }
        });
        m_log.push_back(m_name + " built");
    }
    void onEnter() override { m_log.push_back(m_name + " enter"); }
    void onLeave() override { m_log.push_back(m_name + " leave"); }
    bool onBack() override {
        m_log.push_back(m_name + " back");
        return handlesBack;
    }

    Button* button = nullptr;
    std::function<void()> onClick;
    bool handlesBack = false;

private:
    std::string m_name;
    std::vector<std::string>& m_log;
    ScreenOptions m_options;
};

struct Fixture {
    std::unique_ptr<rhi::Device> device = rhi::createNullDevice();
    std::unique_ptr<UiRenderer> renderer;
    config::SettingsRegistry settings;
    std::unique_ptr<UiContext> context;
    std::unique_ptr<ScreenManager> screens;
    std::vector<std::string> log;

    Fixture() {
        auto created = UiRenderer::create(
            {.device = device.get(), .shaders = rhi::folderShaderReader(VXE_ENGINE_SHADER_DIR)});
        REQUIRE(created.hasValue());
        renderer = std::move(created).value();
        context = UiContext::create({.settings = &settings});
        REQUIRE(settings.set("Interface.UiScale", config::SettingValue{i32{1}}).hasValue());
        screens = std::make_unique<ScreenManager>(*context);
    }

    TestScreen& push(const std::string& name, ScreenOptions options = {}) {
        auto screen = std::make_unique<TestScreen>(name, log, options);
        TestScreen& result = *screen;
        screens->push(std::move(screen));
        return result;
    }
    void frame(UiInput input = {.pointer = {.available = false}}) const {
        screens->update(input.deltaSeconds);
        context->update(640, 480, input, *renderer);
        renderer->begin(640, 480);
        context->draw(*renderer);
    }
    // Back is read by the context, then handed to the screen next frame.
    void back() const {
        frame({.pointer = {.available = false}, .navigation = {.back = true}});
        frame();
    }
    void click(const Widget& widget) const {
        const UiRect& box = widget.rect();
        for (const bool down : {false, true, false}) {
            frame({.pointer = {.x = box.x + 2.0F, .y = box.y + 2.0F, .primaryDown = down}});
        }
    }
};

} // namespace

TEST_CASE("Screens stack: only the top one is shown and takes input, unless it shows the one below") {
    Fixture ui;
    TestScreen& menu = ui.push("menu");
    CHECK(ui.screens->empty()); // Changes wait for the next update.
    ui.frame();
    REQUIRE(ui.screens->size() == 1);
    CHECK(ui.screens->top() == &menu);
    CHECK(ui.log == std::vector<std::string>{"menu built", "menu enter"});
    CHECK(menu.button->rect().width > 0.0F);

    TestScreen& list = ui.push("list");
    ui.frame();
    CHECK(ui.screens->top() == &list);
    CHECK_FALSE(menu.button->parent()->visible()); // Covered: hidden, but kept.
    CHECK(ui.log.back() == "list enter");

    // A pause style screen keeps the one below visible but inert.
    ui.push("pause", {.drawBelow = true, .backdrop = true});
    ui.frame();
    CHECK(list.button->parent()->visible());
    CHECK_FALSE(list.button->parent()->interactive());
    CHECK_FALSE(menu.button->parent()->visible());
    ui.click(*list.button); // Behind the pause screen: nothing reaches it.
    CHECK_FALSE(list.button->hovered());
    CHECK(ui.context->wantsPointer()); // The backdrop blocks the pointer.
    CHECK(ui.screens->wantsCursor());

    ui.screens->pop();
    ui.frame();
    CHECK(ui.screens->top() == &list);
    CHECK(list.button->parent()->interactive());
    CHECK(ui.log.back() == "list enter");
}

TEST_CASE("Back pops the top screen unless it handles Back or is the last one") {
    Fixture ui;
    TestScreen& menu = ui.push("menu");
    TestScreen& settings = ui.push("settings");
    ui.frame();
    CHECK(ui.log == std::vector<std::string>{"menu built", "settings built", "settings enter"});

    settings.handlesBack = true;
    ui.back();
    CHECK(ui.screens->top() == &settings); // Handled by the screen.
    settings.handlesBack = false;
    ui.back();
    CHECK(ui.screens->top() == &menu);
    CHECK(ui.log.back() == "menu enter");
    ui.back();
    CHECK(ui.screens->top() == &menu); // The last screen stays.
    CHECK(ui.log.back() == "menu back");
}

TEST_CASE("A button may replace its own screen, and clear leaves cleanly") {
    Fixture ui;
    TestScreen& menu = ui.push("menu");
    ui.frame();
    menu.onClick = [&ui] {
        auto next = std::make_unique<TestScreen>("worlds", ui.log);
        ui.screens->replace(std::move(next));
    };
    ui.click(*menu.button);
    ui.frame();
    REQUIRE(ui.screens->size() == 1);
    CHECK(ui.log.back() == "worlds enter");

    ui.push("pause", {.drawBelow = true});
    ui.frame();
    ui.log.clear();
    ui.screens->clear();
    ui.frame();
    CHECK(ui.screens->empty());
    CHECK(ui.context->root().children().empty());
    // Only the top screen was entered, so only it leaves.
    CHECK(ui.log == std::vector<std::string>{"pause leave"});
    CHECK_FALSE(ui.screens->wantsCursor());
}
