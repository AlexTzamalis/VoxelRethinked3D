#include "../test_helpers.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/io/file_io.hpp>
#include <vxe/rhi_null/null_device.hpp>
#include <vxe/ui/theme.hpp>
#include <vxe/ui/ui_context.hpp>
#include <vxe/ui/ui_renderer.hpp>
#include <vxe/ui/widgets.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace vxe;
using namespace vxe::ui;

namespace {

// A widget with a fixed content size, to test layout without text.
class Box : public Widget {
public:
    explicit Box(f32 width, f32 height)
        : m_size{width, height} {}
    [[nodiscard]] Size measureContent(const UiEnvironment& /*environment*/,
                                      f32 /*availableWidth*/) const override {
        return m_size;
    }

private:
    Size m_size;
};

void layOut(Widget& root, f32 width, f32 height, const PixelFont* font = nullptr) {
    const Theme theme;
    root.arrange({.theme = theme, .font = font}, {0.0F, 0.0F, width, height});
}

bool same(const UiRect& a, const UiRect& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

// A font where every glyph but the box is 5 pixels wide (advance 6), so
// text widths are easy to compute.
PixelFont blockFont() {
    std::vector<u32> pixels(usize{128} * 128, 0U);
    for (u32 code = 0; code < 256; ++code) {
        const u32 x0 = (code % 16) * 8;
        const u32 y0 = (code / 16) * 8;
        for (u32 y = 0; y < 8; ++y) {
            for (u32 x = 0; x < 5; ++x) {
                pixels[usize{y0 + y} * 128 + x0 + x] = 0xFFFFFFFFU;
            }
        }
    }
    auto font = PixelFont::fromSheet(pixels, 128, 128);
    REQUIRE(font.hasValue());
    return std::move(font).value();
}

} // namespace

TEST_CASE("Columns stack children and stretch them across") {
    Widget root;
    root.style().padding = Edges::all(10.0F);
    root.style().gap = 4.0F;
    auto& a = root.add<Box>(30.0F, 20.0F);
    auto& b = root.add<Box>(50.0F, 10.0F);
    layOut(root, 200.0F, 100.0F);
    CHECK(same(a.rect(), {10.0F, 10.0F, 180.0F, 20.0F}));
    CHECK(same(b.rect(), {10.0F, 34.0F, 180.0F, 10.0F}));
    const Theme theme;
    const Size measured = root.measure({.theme = theme}, kUnbounded);
    CHECK(measured.width == 70.0F);  // 50 + 2 x 10 padding.
    CHECK(measured.height == 54.0F); // 20 + 4 + 10 + 2 x 10.
}

TEST_CASE("Rows share leftover space by grow and center by justify and align") {
    Widget root;
    root.style().direction = Direction::Row;
    root.style().align = Align::Center;
    auto& fixed = root.add<Box>(40.0F, 10.0F);
    auto& one = root.add<Box>(10.0F, 20.0F);
    auto& two = root.add<Box>(10.0F, 20.0F);
    one.style().grow = 1.0F;
    two.style().grow = 3.0F;
    layOut(root, 200.0F, 40.0F);
    // 140 left over: 35 to one, 105 to two.
    CHECK(same(fixed.rect(), {0.0F, 15.0F, 40.0F, 10.0F}));
    CHECK(same(one.rect(), {40.0F, 10.0F, 45.0F, 20.0F}));
    CHECK(same(two.rect(), {85.0F, 10.0F, 115.0F, 20.0F}));

    Widget centered;
    centered.style().direction = Direction::Row;
    centered.style().justify = Justify::Center;
    centered.style().align = Align::End;
    auto& box = centered.add<Box>(50.0F, 10.0F);
    layOut(centered, 200.0F, 40.0F);
    CHECK(same(box.rect(), {75.0F, 30.0F, 50.0F, 10.0F}));

    Widget spread;
    spread.style().direction = Direction::Row;
    spread.style().justify = Justify::SpaceBetween;
    auto& first = spread.add<Box>(20.0F, 10.0F);
    auto& last = spread.add<Box>(20.0F, 10.0F);
    layOut(spread, 100.0F, 10.0F);
    CHECK(first.rect().x == 0.0F);
    CHECK(last.rect().x == 80.0F);
}

TEST_CASE("Too little space shrinks children in proportion, never below their minimum") {
    Widget root;
    root.style().direction = Direction::Row;
    auto& big = root.add<Box>(100.0F, 10.0F);
    auto& small = root.add<Box>(50.0F, 10.0F);
    layOut(root, 120.0F, 10.0F);
    // 30 missing, taken 2 : 1.
    CHECK(big.rect().width == 80.0F);
    CHECK(small.rect().width == 40.0F);

    small.style().minWidth = 48.0F;
    big.style().shrink = 0.0F;
    layOut(root, 120.0F, 10.0F);
    CHECK(big.rect().width == 100.0F);
    CHECK(small.rect().width == 48.0F);
}

TEST_CASE("Fixed sizes, limits and hidden children") {
    Widget root;
    auto& fixed = root.add<Box>(10.0F, 10.0F);
    fixed.style().width = 64.0F;
    fixed.style().height = 32.0F;
    fixed.style().alignSelf = Align::Start;
    auto& limited = root.add<Box>(10.0F, 10.0F);
    limited.style().maxWidth = 50.0F;
    auto& hidden = root.add<Box>(10.0F, 10.0F);
    hidden.setVisible(false);
    auto& after = root.add<Box>(10.0F, 10.0F);
    layOut(root, 200.0F, 200.0F);
    CHECK(same(fixed.rect(), {0.0F, 0.0F, 64.0F, 32.0F}));
    CHECK(limited.rect().width == 50.0F);
    CHECK(hidden.rect().width == 0.0F);
    CHECK(after.rect().y == 42.0F); // The hidden child takes no space.
}

TEST_CASE("Positions snap to whole UI pixels without gaps between neighbors") {
    Widget root;
    root.style().direction = Direction::Row;
    std::vector<Box*> boxes;
    for (int i = 0; i < 3; ++i) {
        boxes.push_back(&root.add<Box>(0.0F, 10.0F));
        boxes.back()->style().grow = 1.0F;
    }
    layOut(root, 100.0F, 10.0F); // 33.33 each.
    f32 x = 0.0F;
    for (const Box* box : boxes) {
        CHECK(box->rect().x == x);
        CHECK(box->rect().width == std::round(box->rect().width));
        x += box->rect().width;
    }
    CHECK(x == 100.0F);
}

TEST_CASE("Labels wrap to the width a column gives them") {
    const PixelFont font = blockFont();
    Widget root;
    auto& label = root.add<Label>("aaaa aaaa aaaa"); // Each word 23 wide, a space 4.
    label.setWrap(true);
    layOut(root, 60.0F, 100.0F, &font);
    CHECK(label.rect().height == kFontLineHeight + kFontCellPixels); // Two lines: "aaaa aaaa" fits 51.
    label.setWrap(false);
    layOut(root, 60.0F, 100.0F, &font);
    CHECK(label.rect().height == kFontCellPixels);
}

TEST_CASE("Labels scale their text in whole steps") {
    const PixelFont font = blockFont();
    Widget root;
    root.style().align = Align::Start;
    auto& title = root.add<Label>("Title"); // 5 glyphs: 29 font pixels wide, 8 high.
    layOut(root, 300.0F, 100.0F, &font);
    CHECK(title.rect().width == 29.0F);
    CHECK(title.rect().height == kFontCellPixels);
    title.setTextScale(4);
    layOut(root, 300.0F, 100.0F, &font);
    CHECK(title.rect().width == 116.0F);
    CHECK(title.rect().height == 32.0F);
}

TEST_CASE("Changes request a new layout through the root") {
    Widget root;
    auto& row = root.add<Widget>();
    auto& label = row.add<Label>("one");
    (void)root.takeLayoutRequest();
    CHECK_FALSE(root.takeLayoutRequest());
    label.setText("two");
    CHECK(root.takeLayoutRequest());
    label.setText("two"); // Same text: nothing to do.
    CHECK_FALSE(root.takeLayoutRequest());
    row.clearChildren();
    CHECK(root.takeLayoutRequest());
    CHECK(root.takeStructureChange());
}

TEST_CASE("Themes read colors and sizes and report bad entries") {
    u32 color = 0;
    CHECK(parseColor("#FF8000", color));
    CHECK(color == rgba(255, 128, 0));
    CHECK(parseColor("#10203040", color));
    CHECK(color == rgba(0x10, 0x20, 0x30, 0x40));
    CHECK_FALSE(parseColor("FF8000", color));
    CHECK_FALSE(parseColor("#GG0000", color));

    std::vector<std::string> warnings;
    const std::string text = R"(
[colors]
button = "#102030"
focus = "red"
sparkle = "#FFFFFF"
[metrics]
button_height = 24
gap = -3
[extra]
)";
    auto theme = Theme::parse(text, warnings);
    REQUIRE(theme.hasValue());
    CHECK(theme.value().colors.button == rgba(0x10, 0x20, 0x30));
    CHECK(theme.value().colors.focus == Theme{}.colors.focus); // Kept on a bad value.
    CHECK(theme.value().metrics.buttonHeight == 24.0F);
    CHECK(theme.value().metrics.gap == Theme{}.metrics.gap);
    CHECK(warnings.size() == 4); // focus, sparkle, gap, [extra].
    CHECK_FALSE(Theme::parse("[colors", warnings).hasValue());
}

TEST_CASE("The default pack's theme loads without warnings") {
    const auto bytes = io::readFile(std::filesystem::path(VXE_SOURCE_DIR) / "textures/default/ui/theme.toml");
    REQUIRE(bytes.hasValue());
    std::vector<std::string> warnings;
    const auto theme = Theme::parse(
        std::string_view(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size()),
        warnings);
    REQUIRE(theme.hasValue());
    CHECK(warnings.empty());
    // The file states the built in defaults, so a pack without a theme looks the same.
    CHECK(theme.value().colors.panel == Theme{}.colors.panel);
    CHECK(theme.value().colors.focus == Theme{}.colors.focus);
}

TEST_CASE("The automatic UI scale is the largest whole one that fits 320 x 240") {
    CHECK(UiContext::autoScale(640, 480) == 2);
    CHECK(UiContext::autoScale(1920, 1080) == 4);
    CHECK(UiContext::autoScale(2560, 1440) == 6);
    CHECK(UiContext::autoScale(300, 200) == 1);
    CHECK(UiContext::autoScale(10000, 10000) == 8);
}

TEST_CASE("The pointer hovers, presses and clicks buttons") {
    const auto device = rhi::createNullDevice();
    auto renderer = UiRenderer::create({.device = device.get(),
                                        .shaders = rhi::folderShaderReader(VXE_ENGINE_SHADER_DIR),
                                        .targetFormat = rhi::Format::RGBA8Unorm});
    REQUIRE(renderer.hasValue());
    config::SettingsRegistry settings;
    auto context = UiContext::create({.settings = &settings});
    REQUIRE(settings.set("Interface.UiScale", config::SettingValue{i32{2}}).hasValue());

    int clicks = 0;
    Widget& root = context->root();
    auto& panel = root.add<Panel>();
    panel.style().alignSelf = Align::Start;
    auto& button = panel.add<Button>("Go", [&clicks] { ++clicks; });
    auto& disabled = panel.add<Button>("Off", [&clicks] { clicks += 100; });
    disabled.setEnabled(false);

    const auto frame = [&](f32 x, f32 y, bool down, bool available = true) {
        context->update(640, 480, {.pointer = {.x = x, .y = y, .primaryDown = down, .available = available}},
                        *renderer.value());
    };
    frame(0.0F, 0.0F, false);
    CHECK(context->scale() == 2);
    // The button in window pixels: UI rect times the scale.
    const UiRect box = button.rect();
    const f32 cx = (box.x + box.width / 2.0F) * 2.0F;
    const f32 cy = (box.y + box.height / 2.0F) * 2.0F;
    CHECK(box.height == Theme{}.metrics.buttonHeight);

    frame(cx, cy, false);
    CHECK(button.hovered());
    CHECK(context->wantsPointer());
    frame(cx, cy, true);
    CHECK(button.pressed());
    frame(cx, cy, false);
    CHECK(clicks == 1);
    CHECK_FALSE(button.pressed());

    // Pressed, dragged away and released: no click.
    frame(cx, cy, true);
    frame(1000.0F, 900.0F, false);
    CHECK(clicks == 1);
    CHECK_FALSE(button.hovered());
    CHECK_FALSE(context->wantsPointer());

    // Disabled buttons never react; the panel still blocks the pointer.
    const UiRect off = disabled.rect();
    const f32 ox = (off.x + 2.0F) * 2.0F;
    const f32 oy = (off.y + 2.0F) * 2.0F;
    frame(ox, oy, false);
    frame(ox, oy, true);
    frame(ox, oy, false);
    CHECK(clicks == 1);
    CHECK(context->wantsPointer());

    // A captured mouse belongs to the game.
    frame(cx, cy, false, false);
    CHECK_FALSE(button.hovered());
    CHECK_FALSE(context->wantsPointer());

    // A click may tear down the tree it came from.
    button.setOnClick([&root] { root.clearChildren(); });
    frame(cx, cy, false);
    frame(cx, cy, true);
    frame(cx, cy, false);
    CHECK(root.children().empty());
    frame(cx, cy, true); // Nothing left to press; must not touch the old button.
    frame(cx, cy, false);
    CHECK(context->hovered() == nullptr);

    // Drawing adds the tree to the renderer's frame.
    auto& again = root.add<Panel>();
    again.add<Label>("hello");
    frame(0.0F, 0.0F, false);
    renderer.value()->begin(640, 480);
    context->draw(*renderer.value());
    CHECK(renderer.value()->quadCount() >= 2); // Panel fill and border; text needs a font.
}
