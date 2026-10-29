#include <vxe/config/settings_file.hpp>

#include <doctest/doctest.h>

#include <array>
#include <filesystem>
#include <string>
#include <system_error>

using vxe::ErrorCode;
using vxe::config::BoolSettingDesc;
using vxe::config::EnumSettingDesc;
using vxe::config::FloatSettingDesc;
using vxe::config::IntSettingDesc;
using vxe::config::Scope;
using vxe::config::Setting;
using vxe::config::SettingsRegistry;
using vxe::config::SettingValue;
using vxe::config::StringSettingDesc;

namespace {

enum class PresentMode : vxe::u8 {
    Immediate,
    Vsync,
    Adaptive
};

constexpr std::array<EnumSettingDesc<PresentMode>::Option, 3> kPresentModes{{
    {.value = PresentMode::Immediate, .name = "Immediate", .displayName = "Uncapped"},
    {.value = PresentMode::Vsync, .name = "Vsync", .displayName = "Vertical sync"},
    {.value = PresentMode::Adaptive, .name = "Adaptive", .displayName = "Adaptive vertical sync"},
}};

// A small but representative set of settings, registered the same way in
// every test so files written by one registry load into another.
struct TestSettings {
    Setting<vxe::i32> renderDistance;
    Setting<vxe::f32> fov;
    Setting<bool> fancyLeaves;
    Setting<PresentMode> presentMode;
    Setting<std::string> language;
    Setting<vxe::i32> worldTick;

    explicit TestSettings(SettingsRegistry& registry) {
        IntSettingDesc distance;
        distance.id = "World.Streaming.RenderDistance";
        distance.displayName = "Render distance";
        distance.description = "Chunks loaded around the player.";
        distance.defaultValue = 16;
        distance.min = 2;
        distance.max = 64;
        renderDistance = registry.registerInt(distance);

        FloatSettingDesc fovDesc;
        fovDesc.id = "Gameplay.FieldOfView";
        fovDesc.displayName = "Field of view";
        fovDesc.defaultValue = 70.0F;
        fovDesc.min = 30.0F;
        fovDesc.max = 110.0F;
        fov = registry.registerFloat(fovDesc);

        BoolSettingDesc leaves;
        leaves.id = "Graphics.Terrain.FancyLeaves";
        leaves.defaultValue = true;
        fancyLeaves = registry.registerBool(leaves);

        EnumSettingDesc<PresentMode> mode;
        mode.id = "Video.PresentMode";
        mode.defaultValue = PresentMode::Vsync;
        mode.options = kPresentModes;
        presentMode = registry.registerEnum(mode);

        StringSettingDesc lang;
        lang.id = "Interface.Language";
        lang.defaultValue = "en";
        language = registry.registerString(lang);

        IntSettingDesc tick;
        tick.id = "Server.TickRate";
        tick.defaultValue = 30;
        tick.min = 10;
        tick.max = 120;
        tick.scope = Scope::PerWorld;
        worldTick = registry.registerInt(tick);
    }
};

} // namespace

TEST_CASE("Saved settings load back into a fresh registry") {
    SettingsRegistry original;
    const TestSettings a(original);
    REQUIRE(a.renderDistance.set(32).hasValue());
    REQUIRE(a.fov.set(92.5F).hasValue());
    REQUIRE(a.fancyLeaves.set(false).hasValue());
    REQUIRE(a.presentMode.set(PresentMode::Adaptive).hasValue());
    REQUIRE(a.language.set("el \"Greek\"").hasValue()); // Quotes must be escaped.
    REQUIRE(a.worldTick.set(60).hasValue());

    const std::string text = vxe::config::saveSettingsToString(original);
    CHECK(text.find("[World.Streaming]") != std::string::npos);
    CHECK(text.find("# Render distance: Chunks loaded around the player.") != std::string::npos);
    CHECK(text.find("PresentMode = \"Adaptive\"") != std::string::npos);
    CHECK(text.find("TickRate") == std::string::npos); // Per world settings go elsewhere.

    SettingsRegistry loaded;
    const TestSettings b(loaded);
    const auto report = vxe::config::loadSettingsFromString(loaded, text);
    REQUIRE(report.hasValue());
    CHECK(report.value().warnings.empty());
    CHECK(b.renderDistance.get() == 32);
    CHECK(b.fov.get() == 92.5F);
    CHECK_FALSE(b.fancyLeaves.get());
    CHECK(b.presentMode.get() == PresentMode::Adaptive);
    CHECK(b.language.get() == "el \"Greek\"");
    CHECK(b.worldTick.get() == 30); // Not in the global file.

    // The per world scope round trips through its own file.
    const std::string world = vxe::config::saveSettingsToString(original, Scope::PerWorld);
    REQUIRE(vxe::config::loadSettingsFromString(loaded, world, Scope::PerWorld).hasValue());
    CHECK(b.worldTick.get() == 60);
}

TEST_CASE("Settings registered after loading take their saved value, and save once") {
    SettingsRegistry original;
    const TestSettings a(original);
    REQUIRE(a.renderDistance.set(32).hasValue());
    REQUIRE(a.presentMode.set(PresentMode::Adaptive).hasValue());
    const std::string text = vxe::config::saveSettingsToString(original);

    // Loaded before anything registered: every key is unknown for now.
    SettingsRegistry later;
    REQUIRE(vxe::config::loadSettingsFromString(later, text).hasValue());
    CHECK_FALSE(later.unknown(Scope::Global).empty());
    const TestSettings b(later);
    CHECK(b.renderDistance.get() == 32);
    CHECK(b.presentMode.get() == PresentMode::Adaptive);
    CHECK(later.unknown(Scope::Global).empty());
    const std::string saved = vxe::config::saveSettingsToString(later);
    CHECK(saved.find("RenderDistance") == saved.rfind("RenderDistance"));
    CHECK(vxe::config::loadSettingsFromString(later, saved).hasValue()); // Valid TOML: no key twice.
}

TEST_CASE("A hand edited file loads, with warnings instead of failures") {
    constexpr std::string_view kHandEdited = R"(
# My settings, edited in Notepad.
[Gameplay]
FieldOfView = 90          # an integer where a float is expected is fine

[World.Streaming]
RenderDistance = 500      # too far, clamped to 64

[Video]
PresentMode = "vsync"     # case does not matter
OldShadowQuality = "High" # an option that no longer exists in this table

[Graphics.Terrain]
FancyLeaves = "yes"       # wrong type: warning, value kept

[Interface]
Language = "de"

[Removed.Feature]
Values = [1, 2, 3]
)";
    SettingsRegistry registry;
    const TestSettings s(registry);
    const auto report = vxe::config::loadSettingsFromString(registry, kHandEdited);
    REQUIRE(report.hasValue());
    CHECK(s.fov.get() == 90.0F);
    CHECK(s.renderDistance.get() == 64);
    CHECK(s.presentMode.get() == PresentMode::Vsync);
    CHECK(s.fancyLeaves.get()); // Kept its default.
    CHECK(s.language.get() == "de");
    REQUIRE(report.value().warnings.size() == 1);
    CHECK(report.value().warnings[0].find("Graphics.Terrain.FancyLeaves") != std::string::npos);

    // Unknown keys survive a save, untouched.
    const std::string saved = vxe::config::saveSettingsToString(registry);
    // toml++ may write the kept string with single quotes; both are valid TOML.
    CHECK(saved.find("OldShadowQuality = ") != std::string::npos);
    CHECK(saved.find("High") != std::string::npos);
    CHECK(saved.find("[Removed.Feature]") != std::string::npos);
    CHECK(saved.find("Values = [ 1, 2, 3 ]") != std::string::npos);

    SettingsRegistry again;
    const TestSettings t(again);
    REQUIRE(vxe::config::loadSettingsFromString(again, saved).hasValue());
    const auto kept = again.unknown(Scope::Global);
    REQUIRE(kept.size() == 2);
    CHECK(kept[1].first == "Video.OldShadowQuality");
    CHECK(kept[1].second.find("High") != std::string::npos);
}

TEST_CASE("Invalid TOML fails with a line number") {
    SettingsRegistry registry;
    const TestSettings s(registry);
    const auto result = vxe::config::loadSettingsFromString(registry, "[Video]\nPresentMode = = 3\n");
    REQUIRE(result.hasError());
    CHECK(result.error().code() == ErrorCode::ParseError);
    CHECK(result.error().message().find("line 2") != std::string::npos);
    CHECK(s.presentMode.get() == PresentMode::Vsync);
}

TEST_CASE("Settings files on disk, and a missing file") {
    const auto folder = std::filesystem::temp_directory_path() / "vxe_tests" / "settings_file";
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);

    SettingsRegistry registry;
    const TestSettings s(registry);
    CHECK(vxe::config::loadSettingsFromFile(registry, folder / "settings.toml").error().code() ==
          ErrorCode::NotFound);

    REQUIRE(s.renderDistance.set(48).hasValue());
    REQUIRE(vxe::config::saveSettingsToFile(registry, folder / "settings.toml").hasValue());

    SettingsRegistry loaded;
    const TestSettings t(loaded);
    REQUIRE(vxe::config::loadSettingsFromFile(loaded, folder / "settings.toml").hasValue());
    CHECK(t.renderDistance.get() == 48);
}

TEST_CASE("Presets set groups of values and are recognized again") {
    SettingsRegistry registry;
    const TestSettings s(registry);
    registry.definePreset("Graphics", "Low",
                          {{"World.Streaming.RenderDistance", SettingValue(vxe::i32{8})},
                           {"Graphics.Terrain.FancyLeaves", SettingValue(false)}});
    registry.definePreset("Graphics", "High",
                          {{"World.Streaming.RenderDistance", SettingValue(vxe::i32{32})},
                           {"Graphics.Terrain.FancyLeaves", SettingValue(true)}});
    CHECK(registry.presetNames("Graphics") == std::vector<std::string>{"Low", "High"});
    CHECK(registry.currentPreset("Graphics") == "Custom");

    REQUIRE(registry.applyPreset("Graphics", "Low").hasValue());
    CHECK(s.renderDistance.get() == 8);
    CHECK_FALSE(s.fancyLeaves.get());
    CHECK(registry.currentPreset("Graphics") == "Low");

    REQUIRE(s.renderDistance.set(9).hasValue()); // One tweak makes it custom.
    CHECK(registry.currentPreset("Graphics") == "Custom");

    CHECK(registry.applyPreset("Graphics", "Ultra").error().code() == ErrorCode::NotFound);
    registry.definePreset("Graphics", "Broken", {{"No.Such.Setting", SettingValue(true)}});
    CHECK(registry.applyPreset("Graphics", "Broken").hasError());
}
