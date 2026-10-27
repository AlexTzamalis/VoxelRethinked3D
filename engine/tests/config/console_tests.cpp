#include <vxe/config/console.hpp>

#include <doctest/doctest.h>

#include <array>
#include <string>
#include <vector>

using vxe::config::CommandDesc;
using vxe::config::CommandOutput;
using vxe::config::CommandRegistry;
using vxe::config::EnumSettingDesc;
using vxe::config::Setting;
using vxe::config::SettingsRegistry;

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

// A registry with a few settings and the settings commands, like the game has.
struct Console {
    SettingsRegistry settings;
    CommandRegistry commands;
    Setting<vxe::i32> lodDistance;
    Setting<vxe::i32> lodLevels;
    Setting<vxe::f32> fov;
    Setting<bool> wireframe;
    Setting<PresentMode> presentMode;
    Setting<std::string> playerName;

    Console() {
        vxe::config::IntSettingDesc distance;
        distance.id = "Graphics.LOD.MaxDistance";
        distance.displayName = "LOD distance";
        distance.defaultValue = 16000;
        distance.min = 1000;
        distance.max = 64000;
        lodDistance = settings.registerInt(distance);

        vxe::config::IntSettingDesc levels;
        levels.id = "Graphics.LOD.Levels";
        levels.defaultValue = 6;
        levels.min = 1;
        levels.max = 10;
        lodLevels = settings.registerInt(levels);

        vxe::config::FloatSettingDesc fovDesc;
        fovDesc.id = "Gameplay.FieldOfView";
        fovDesc.displayName = "Field of view";
        fovDesc.defaultValue = 70.0F;
        fovDesc.min = 30.0F;
        fovDesc.max = 110.0F;
        fov = settings.registerFloat(fovDesc);

        vxe::config::BoolSettingDesc wire;
        wire.id = "Debug.Wireframe";
        wireframe = settings.registerBool(wire);

        EnumSettingDesc<PresentMode> mode;
        mode.id = "Video.PresentMode";
        mode.defaultValue = PresentMode::Vsync;
        mode.options = kPresentModes;
        presentMode = settings.registerEnum(mode);

        vxe::config::StringSettingDesc name;
        name.id = "Interface.PlayerName";
        name.defaultValue = "Player";
        playerName = settings.registerString(name);

        vxe::config::registerSettingsCommands(commands, settings);
    }
};

std::string firstLine(const CommandOutput& output) {
    return output.lines.empty() ? std::string() : output.lines.front();
}

} // namespace

TEST_CASE("Command lines are split into words, with quotes") {
    using vxe::config::tokenizeCommandLine;
    CHECK(tokenizeCommandLine("set  Video.PresentMode   Vsync") ==
          std::vector<std::string>{"set", "Video.PresentMode", "Vsync"});
    CHECK(tokenizeCommandLine(R"(say "hello world" "a \"quote\"" "")") ==
          std::vector<std::string>{"say", "hello world", "a \"quote\"", ""});
    CHECK(tokenizeCommandLine("   ").empty());
}

TEST_CASE("set and get change and show settings") {
    Console console;
    CommandOutput output = console.commands.execute("set Graphics.LOD.MaxDistance 32000");
    CHECK(output.success);
    CHECK(console.lodDistance.get() == 32000);
    CHECK(firstLine(output) == "Graphics.LOD.MaxDistance = 32000 (default 16000)");

    CHECK(console.commands.execute("set Video.PresentMode adaptive").success); // Any case.
    CHECK(console.presentMode.get() == PresentMode::Adaptive);
    CHECK(console.commands.execute("set Debug.Wireframe on").success);
    CHECK(console.wireframe.get());
    CHECK(console.commands.execute("set Gameplay.FieldOfView 95.5").success);
    CHECK(console.fov.get() == 95.5F);
    CHECK(console.commands.execute("set Interface.PlayerName Alex the Builder").success); // Rest of the line.
    CHECK(console.playerName.get() == "Alex the Builder");

    CHECK(firstLine(console.commands.execute("get Video.PresentMode")) ==
          "Video.PresentMode = Adaptive (default Vsync)");
    CHECK(firstLine(console.commands.execute("get Graphics.LOD.Levels")) == "Graphics.LOD.Levels = 6");
}

TEST_CASE("Bad input fails with a helpful message and changes nothing") {
    Console console;
    CommandOutput badNumber = console.commands.execute("set Graphics.LOD.MaxDistance far");
    CHECK_FALSE(badNumber.success);
    CHECK(firstLine(badNumber).find("whole number") != std::string::npos);
    CHECK(console.lodDistance.get() == 16000);

    CommandOutput badOption = console.commands.execute("set Video.PresentMode Turbo");
    CHECK_FALSE(badOption.success);
    CHECK(firstLine(badOption).find("Immediate, Vsync, Adaptive") != std::string::npos);

    CHECK_FALSE(console.commands.execute("set No.Such.Setting 1").success);
    CHECK_FALSE(console.commands.execute("set Graphics.LOD.MaxDistance").success);
    CommandOutput unknown = console.commands.execute("sett x");
    CHECK_FALSE(unknown.success);
    CHECK(firstLine(unknown).find("Did you mean: set") != std::string::npos);
    CHECK(console.commands.execute("").success); // Empty lines are fine.
}

TEST_CASE("reset handles one setting, a category with a wildcard, and everything") {
    Console console;
    REQUIRE(console.lodDistance.set(50000).hasValue());
    REQUIRE(console.lodLevels.set(9).hasValue());
    REQUIRE(console.fov.set(100.0F).hasValue());

    CHECK(console.commands.execute("reset Gameplay.FieldOfView").success);
    CHECK(console.fov.get() == 70.0F);

    const CommandOutput category = console.commands.execute("reset Graphics.LOD.*");
    CHECK(firstLine(category) == "Reset 2 settings under Graphics.LOD.*");
    CHECK(console.lodDistance.get() == 16000);
    CHECK(console.lodLevels.get() == 6);

    REQUIRE(console.wireframe.set(true).hasValue());
    CHECK(console.commands.execute("reset *").success);
    CHECK_FALSE(console.wireframe.get());
}

TEST_CASE("find and list search the settings") {
    Console console;
    const CommandOutput lod = console.commands.execute("find lod");
    CHECK(lod.lines.size() == 2);
    const CommandOutput byName = console.commands.execute("find \"field of\"");
    REQUIRE(byName.lines.size() == 1);
    CHECK(byName.lines[0].starts_with("Gameplay.FieldOfView"));

    CHECK(console.commands.execute("list").lines.size() == 6);
    CHECK(console.commands.execute("list Graphics").lines.size() == 2);
    CHECK(console.commands.execute("list Graph").lines.empty()); // Whole category names only.
    CHECK(console.commands.execute("help").lines.size() == 6);
}

TEST_CASE("Autocomplete offers commands, setting ids and values") {
    const Console console;
    CHECK(console.commands.complete("se") == std::vector<std::string>{"set"});
    CHECK(console.commands.complete("set Graphics.LOD.") ==
          std::vector<std::string>{"Graphics.LOD.Levels", "Graphics.LOD.MaxDistance"});
    CHECK(console.commands.complete("set video.pre") == std::vector<std::string>{"Video.PresentMode"});
    CHECK(console.commands.complete("set Video.PresentMode ") ==
          std::vector<std::string>{"Adaptive", "Immediate", "Vsync"});
    CHECK(console.commands.complete("set Debug.Wireframe o") == std::vector<std::string>{"off", "on"});
    CHECK(console.commands.complete("list Gr") == std::vector<std::string>{"Graphics.LOD"});
    CHECK(console.commands.complete("unknowncommand x").empty());
}

TEST_CASE("Custom commands can be added") {
    CommandRegistry commands;
    int calls = 0;
    commands.registerCommand(CommandDesc{.name = "ping",
                                         .usage = "ping",
                                         .description = "Answers pong.",
                                         .handler =
                                             [&calls](std::span<const std::string>) {
                                                 ++calls;
                                                 return CommandOutput{.success = true, .lines = {"pong"}};
                                             },
                                         .completer = nullptr});
    CHECK(commands.hasCommand("ping"));
    CHECK(firstLine(commands.execute("PING")) == "pong"); // Command names ignore case.
    CHECK(calls == 1);
}
