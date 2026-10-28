#pragma once

#include <vxe/config/console.hpp>
#include <vxe/config/settings.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/types.hpp>
#include <vxe/input/input.hpp>

#include <array>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct ImGuiInputTextCallbackData;

namespace vxe::debug {

struct ConsoleDesc {
    config::CommandRegistry* commands = nullptr;
    config::SettingsRegistry* settings = nullptr;
    input::InputSystem* input = nullptr;
    stats::StatRegistry* stats = nullptr; // Optional: counts commands run.
    // The always active context the toggle key lives in (usually the
    // overlay's Debug context). If invalid, the console registers its own.
    input::InputContextId toggleContext{};
};

// The in game developer console (Section 16.5, E7.4): an ImGui window over
// the console commands (set, get, reset, find, list, help and any command a
// system adds).
//
// The Grave key (left of 1) opens and closes it. While open it pushes the
// "Console" input context, which blocks gameplay input, so typed letters
// never move the player. In the input field Enter runs the line, Up and Down
// walk the history and Tab completes the word being typed (a single match is
// filled in, several are listed and their common start is filled in).
//
// Per frame, between DebugUi::beginFrame and DebugUi::endFrame: update().
//
// Thread safety: main thread only.
class DeveloperConsole {
public:
    enum class LineKind : u8 {
        Input,
        Output,
        Error
    };
    struct Line {
        std::string text;
        LineKind kind = LineKind::Output;
    };

    explicit DeveloperConsole(const ConsoleDesc& desc);
    ~DeveloperConsole();
    DeveloperConsole(const DeveloperConsole&) = delete;
    DeveloperConsole& operator=(const DeveloperConsole&) = delete;
    DeveloperConsole(DeveloperConsole&&) = delete;
    DeveloperConsole& operator=(DeveloperConsole&&) = delete;

    // Runs a line as if it was typed: echoes it, adds it to the history and
    // prints what the command printed. Returns whether the command worked.
    bool execute(std::string_view line);

    // Reads the toggle key and draws the window while open: handleInput()
    // then draw(). The parts are separate so input works without a UI frame.
    void update();
    void handleInput();
    // Draws the window if open. Needs an ImGui frame.
    void draw();

    [[nodiscard]] bool isOpen() const noexcept { return m_open; }
    void setOpen(bool open);

    // The line after Tab: the word being typed completed as far as the
    // candidates agree. Lists the candidates when there are several.
    [[nodiscard]] std::string completeLine(std::string_view line);
    // Walking the history from the input field. Up goes back, Down forward;
    // Down past the newest entry returns an empty line.
    [[nodiscard]] std::optional<std::string> historyUp();
    [[nodiscard]] std::optional<std::string> historyDown();

    [[nodiscard]] const std::deque<Line>& lines() const noexcept { return m_lines; }
    [[nodiscard]] const std::vector<std::string>& history() const noexcept { return m_history; }

private:
    void print(std::string text, LineKind kind);
    static int inputCallback(ImGuiInputTextCallbackData* data);

    config::CommandRegistry& m_commands;
    input::InputSystem& m_input;
    stats::StatRegistry* m_stats;

    config::Setting<i32> m_maxLines;
    config::Setting<i32> m_historySize;
    input::InputContextId m_context;          // Blocks gameplay while open.
    input::InputContextId m_ownToggleContext; // Only when no toggle context was given.
    input::ActionId m_toggle;
    stats::StatId m_commandsRun;

    bool m_open = false;
    bool m_focusInput = false;
    bool m_scrollToBottom = false;
    std::deque<Line> m_lines;
    std::vector<std::string> m_history;
    std::optional<usize> m_historyCursor; // Index into m_history while walking it.
    std::array<char, 512> m_inputBuffer{};
};

} // namespace vxe::debug
