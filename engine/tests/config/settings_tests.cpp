#include <vxe/config/settings.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using vxe::ErrorCode;
using vxe::config::ApplyMode;
using vxe::config::BoolSettingDesc;
using vxe::config::EnumSettingDesc;
using vxe::config::FloatSettingDesc;
using vxe::config::IntSettingDesc;
using vxe::config::Setting;
using vxe::config::SettingsRegistry;
using vxe::config::SettingType;
using vxe::config::SettingValue;
using vxe::config::StringSettingDesc;
using vxe::config::Subscription;

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

Setting<vxe::i32> registerRenderDistance(SettingsRegistry& registry) {
    IntSettingDesc desc;
    desc.id = "World.Streaming.RenderDistance";
    desc.displayName = "Render distance";
    desc.description = "Chunks loaded around the player.";
    desc.defaultValue = 16;
    desc.min = 2;
    desc.max = 64;
    return registry.registerInt(desc);
}

Setting<vxe::f32> registerFov(SettingsRegistry& registry) {
    FloatSettingDesc desc;
    desc.id = "Gameplay.FieldOfView";
    desc.displayName = "Field of view";
    desc.defaultValue = 70.0F;
    desc.min = 30.0F;
    desc.max = 110.0F;
    desc.step = 1.0F;
    return registry.registerFloat(desc);
}

Setting<PresentMode> registerPresentMode(SettingsRegistry& registry) {
    EnumSettingDesc<PresentMode> desc;
    desc.id = "Video.PresentMode";
    desc.displayName = "Present mode";
    desc.defaultValue = PresentMode::Vsync;
    desc.options = kPresentModes;
    return registry.registerEnum(desc);
}

} // namespace

TEST_CASE("Settings start at their default and keep their metadata") {
    SettingsRegistry registry;
    const Setting<vxe::i32> distance = registerRenderDistance(registry);
    CHECK(distance.get() == 16);
    CHECK(distance.info().id == "World.Streaming.RenderDistance");
    CHECK(distance.info().category == "World.Streaming");
    CHECK(distance.info().type == SettingType::Int);
    CHECK(distance.info().applyMode == ApplyMode::Live);
    CHECK(registry.isDefault("World.Streaming.RenderDistance"));

    BoolSettingDesc wireframe;
    wireframe.id = "Debug.Wireframe";
    wireframe.applyMode = ApplyMode::Live;
    CHECK_FALSE(registry.registerBool(wireframe).get());

    StringSettingDesc language;
    language.id = "Interface.Language";
    language.defaultValue = "en";
    CHECK(registry.registerString(language).get() == "en");
}

TEST_CASE("Numbers are clamped to their range") {
    SettingsRegistry registry;
    const Setting<vxe::i32> distance = registerRenderDistance(registry);
    const Setting<vxe::f32> fov = registerFov(registry);

    REQUIRE(distance.set(200).hasValue());
    CHECK(distance.get() == 64);
    REQUIRE(distance.set(-5).hasValue());
    CHECK(distance.get() == 2);

    REQUIRE(fov.set(500.0F).hasValue());
    CHECK(fov.get() == 110.0F);
    REQUIRE(fov.set(90.5F).hasValue());
    CHECK(fov.get() == 90.5F);
}

TEST_CASE("Invalid values are rejected and leave the setting unchanged") {
    SettingsRegistry registry;
    const Setting<vxe::f32> fov = registerFov(registry);
    const Setting<PresentMode> mode = registerPresentMode(registry);

    CHECK(fov.set(std::numeric_limits<vxe::f32>::quiet_NaN()).hasError());
    CHECK(fov.set(std::numeric_limits<vxe::f32>::infinity()).hasError());
    CHECK(fov.get() == 70.0F);

    CHECK(mode.get() == PresentMode::Vsync);
    REQUIRE(mode.set(PresentMode::Adaptive).hasValue());
    CHECK(mode.get() == PresentMode::Adaptive);
    CHECK(registry.set("Video.PresentMode", SettingValue(vxe::i32{7})).error().code() ==
          ErrorCode::InvalidArgument);
    CHECK(mode.get() == PresentMode::Adaptive);

    // Wrong type and unknown ids by name.
    CHECK(registry.set("Gameplay.FieldOfView", SettingValue(true)).error().code() ==
          ErrorCode::InvalidArgument);
    CHECK(registry.set("No.Such.Setting", SettingValue(true)).error().code() == ErrorCode::NotFound);

    StringSettingDesc name;
    name.id = "Interface.PlayerName";
    name.maxLength = 4;
    const Setting<std::string> player = registry.registerString(name);
    CHECK(player.set("toolong").hasError());
    CHECK(player.set("Alex").hasValue());
}

TEST_CASE("Change callbacks run on dispatch with old and new values") {
    SettingsRegistry registry;
    const Setting<vxe::i32> distance = registerRenderDistance(registry);
    std::vector<std::pair<vxe::i32, vxe::i32>> seen;
    const Subscription subscription =
        distance.onChanged([&seen](const vxe::i32& oldValue, const vxe::i32& newValue) {
            seen.emplace_back(oldValue, newValue);
        });

    REQUIRE(distance.set(32).hasValue());
    CHECK(distance.get() == 32); // Readers see the value right away...
    CHECK(seen.empty());         // ...but callbacks wait for the main thread.

    CHECK(registry.dispatchChanges() == 1);
    REQUIRE(seen.size() == 1);
    CHECK(seen[0] == std::pair{16, 32});

    // Several changes in one frame become one callback, oldest to newest.
    REQUIRE(distance.set(8).hasValue());
    REQUIRE(distance.set(12).hasValue());
    CHECK(registry.dispatchChanges() == 1);
    CHECK(seen.back() == std::pair{32, 12});

    // Setting the current value again is not a change.
    REQUIRE(distance.set(12).hasValue());
    CHECK(registry.dispatchChanges() == 0);

    // A change that ends where it started produces no callback.
    REQUIRE(distance.set(20).hasValue());
    REQUIRE(distance.set(12).hasValue());
    CHECK(registry.dispatchChanges() == 0);
}

TEST_CASE("Destroyed subscriptions stop callbacks") {
    SettingsRegistry registry;
    const Setting<vxe::f32> fov = registerFov(registry);
    int calls = 0;
    {
        const Subscription scoped = fov.onChanged([&calls](const vxe::f32&, const vxe::f32&) { ++calls; });
        REQUIRE(fov.set(80.0F).hasValue());
        static_cast<void>(registry.dispatchChanges());
    }
    REQUIRE(fov.set(90.0F).hasValue());
    static_cast<void>(registry.dispatchChanges());
    CHECK(calls == 1);
}

TEST_CASE("Writes from other threads are seen at once and dispatched on the main thread") {
    SettingsRegistry registry;
    const Setting<vxe::i32> distance = registerRenderDistance(registry);
    const std::thread::id mainThread = std::this_thread::get_id();
    bool calledOnMain = false;
    const Subscription subscription =
        distance.onChanged([&calledOnMain, mainThread](const vxe::i32&, const vxe::i32&) {
            calledOnMain = std::this_thread::get_id() == mainThread;
        });

    std::thread writer([&distance] { REQUIRE(distance.set(40).hasValue()); });
    writer.join();
    CHECK(distance.get() == 40);
    CHECK(registry.dispatchChanges() == 1);
    CHECK(calledOnMain);
}

TEST_CASE("Generic access, categories and resets") {
    SettingsRegistry registry;
    const Setting<vxe::i32> distance = registerRenderDistance(registry);
    const Setting<vxe::f32> fov = registerFov(registry);
    const Setting<PresentMode> mode = registerPresentMode(registry);

    CHECK(registry.categories() == std::vector<std::string>{"Gameplay", "Video", "World.Streaming"});
    CHECK(registry.all().size() == 3);
    CHECK(registry.find("Video.PresentMode")->options.size() == 3);
    CHECK(registry.find("Missing") == nullptr);

    REQUIRE(registry.set("World.Streaming.RenderDistance", SettingValue(vxe::i32{24})).hasValue());
    CHECK(std::get<vxe::i32>(registry.value("World.Streaming.RenderDistance").value()) == 24);
    CHECK_FALSE(registry.isDefault("World.Streaming.RenderDistance"));

    REQUIRE(fov.set(100.0F).hasValue());
    REQUIRE(mode.set(PresentMode::Immediate).hasValue());
    CHECK(registry.resetCategory("World") == 1); // Prefix: covers World.Streaming.
    CHECK(distance.get() == 16);
    CHECK(registry.reset("Gameplay.FieldOfView").hasValue());
    CHECK(fov.get() == 70.0F);
    mode.reset();
    CHECK(mode.get() == PresentMode::Vsync);
    CHECK(registry.resetCategory("Vid") == 0); // Not a whole category name.
}

TEST_CASE("Registering an id twice returns the same setting") {
    SettingsRegistry registry;
    const Setting<vxe::i32> first = registerRenderDistance(registry);
    REQUIRE(first.set(30).hasValue());
    const Setting<vxe::i32> second = registerRenderDistance(registry);
    CHECK(second.get() == 30);
    CHECK(registry.all().size() == 1);
}
