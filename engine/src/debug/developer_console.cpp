#include "vxe/debug/developer_console.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>

namespace vxe::debug {
namespace {

constexpr u16 kScancodeGrave = 53; // The key left of 1 (Section 17.5: Console).

ImVec4 lineColor(DeveloperConsole::LineKind kind) {
    switch (kind) {
    case DeveloperConsole::LineKind::Input:
        return {0.55F, 0.75F, 1.0F, 1.0F};
    case DeveloperConsole::LineKind::Error:
        return {1.0F, 0.45F, 0.4F, 1.0F};
    case DeveloperConsole::LineKind::Output:
        break;
    }
    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

// The longest start every candidate shares, compared without case because
// completion matches setting names without case ("video.pre").
std::string commonPrefix(const std::vector<std::string>& candidates) {
    if (candidates.empty()) {
        return {};
    }
    std::string prefix = candidates.front();
    for (const std::string& candidate : candidates) {
        usize length = 0;
        while (length < prefix.size() && length < candidate.size() &&
               std::tolower(static_cast<unsigned char>(prefix[length])) ==
                   std::tolower(static_cast<unsigned char>(candidate[length]))) {
            ++length;
        }
        prefix.resize(length);
    }
    return prefix;
}

} // namespace

DeveloperConsole::DeveloperConsole(const ConsoleDesc& desc)
    : m_commands(*desc.commands)
    , m_input(*desc.input)
    , m_stats(desc.stats) {
    m_maxLines = desc.settings->registerInt({{.id = "Debug.Console.MaxLines",
                                              .displayName = "Console scrollback",
                                              .description = "Lines the developer console keeps.",
                                              .visibility = config::Visibility::Advanced},
                                             1000,
                                             100,
                                             10000,
                                             100});
    m_historySize = desc.settings->registerInt({{.id = "Debug.Console.HistorySize",
                                                 .displayName = "Console history",
                                                 .description = "Commands Up and Down can bring back.",
                                                 .visibility = config::Visibility::Advanced},
                                                100,
                                                10,
                                                1000,
                                                10});
    m_context = m_input.registerContext({.name = "Console", .blocksLowerContexts = true});
    input::InputContextId toggleContext = desc.toggleContext;
    if (!toggleContext.isValid()) {
        m_ownToggleContext = m_input.registerContext({.name = "Console toggle", .alwaysActive = true});
        m_input.pushContext(m_ownToggleContext);
        toggleContext = m_ownToggleContext;
    }
    m_toggle = m_input.registerAction({.id = "debug.console.toggle",
                                       .displayName = "Toggle console",
                                       .category = "Debug",
                                       .context = toggleContext,
                                       .defaults = {input::Binding::key(kScancodeGrave)}});
    if (m_stats != nullptr) {
        m_commandsRun = m_stats
                            ->registerStat({.name = "Debug.Console.Commands",
                                            .description = "Console commands run in the frame.",
                                            .type = stats::StatType::Counter})
                            .value();
    }
    print(
        "Developer console. Type \"help\" for commands, Tab completes, Up and Down bring back earlier lines.",
        LineKind::Output);
}

DeveloperConsole::~DeveloperConsole() {
    if (m_open) {
        m_input.popContext(m_context);
    }
    if (m_ownToggleContext.isValid()) {
        m_input.popContext(m_ownToggleContext);
    }
}

void DeveloperConsole::setOpen(bool open) {
    if (open == m_open) {
        return;
    }
    m_open = open;
    if (open) {
        m_input.pushContext(m_context);
        m_focusInput = true;
        m_scrollToBottom = true;
    } else {
        m_input.popContext(m_context);
    }
}

void DeveloperConsole::print(std::string text, LineKind kind) {
    m_lines.push_back({.text = std::move(text), .kind = kind});
    while (m_lines.size() > static_cast<usize>(std::max(m_maxLines.get(), 1))) {
        m_lines.pop_front();
    }
    m_scrollToBottom = true;
}

bool DeveloperConsole::execute(std::string_view line) {
    const std::string_view trimmed = line.substr(0, line.find_last_not_of(" \t") + 1);
    if (trimmed.find_first_not_of(" \t") == std::string_view::npos) {
        return true;
    }
    print(std::format("> {}", trimmed), LineKind::Input);
    // Repeating the previous command does not fill the history with copies.
    if (m_history.empty() || m_history.back() != trimmed) {
        m_history.emplace_back(trimmed);
        const auto limit = static_cast<usize>(std::max(m_historySize.get(), 1));
        if (m_history.size() > limit) {
            m_history.erase(m_history.begin(),
                            m_history.begin() + static_cast<std::ptrdiff_t>(m_history.size() - limit));
        }
    }
    m_historyCursor.reset();
    const config::CommandOutput output = m_commands.execute(trimmed);
    for (const std::string& text : output.lines) {
        print(text, output.success ? LineKind::Output : LineKind::Error);
    }
    if (m_stats != nullptr) {
        m_stats->add(m_commandsRun);
    }
    return output.success;
}

std::string DeveloperConsole::completeLine(std::string_view line) {
    const std::vector<std::string> candidates = m_commands.complete(line);
    if (candidates.empty()) {
        return std::string(line);
    }
    const usize wordStart = line.find_last_of(' ') == std::string_view::npos ? 0 : line.find_last_of(' ') + 1;
    const std::string start(line.substr(0, wordStart));
    if (candidates.size() == 1) {
        return start + candidates.front() + " ";
    }
    std::string listing;
    for (const std::string& candidate : candidates) {
        listing += candidate;
        listing += "   ";
    }
    print(std::move(listing), LineKind::Output);
    const std::string prefix = commonPrefix(candidates);
    // Keep what was typed when the candidates agree on less than that.
    return prefix.size() > line.size() - wordStart ? start + prefix : std::string(line);
}

std::optional<std::string> DeveloperConsole::historyUp() {
    if (m_history.empty()) {
        return std::nullopt;
    }
    if (!m_historyCursor) {
        m_historyCursor = m_history.size() - 1;
    } else if (*m_historyCursor > 0) {
        --*m_historyCursor;
    }
    return m_history[*m_historyCursor];
}

std::optional<std::string> DeveloperConsole::historyDown() {
    if (!m_historyCursor) {
        return std::nullopt;
    }
    if (*m_historyCursor + 1 >= m_history.size()) {
        m_historyCursor.reset();
        return std::string();
    }
    ++*m_historyCursor;
    return m_history[*m_historyCursor];
}

void DeveloperConsole::update() {
    handleInput();
    draw();
}

void DeveloperConsole::handleInput() {
    if (m_input.isPressed(m_toggle)) {
        setOpen(!m_open);
    }
}

int DeveloperConsole::inputCallback(ImGuiInputTextCallbackData* data) {
    auto* console = static_cast<DeveloperConsole*>(data->UserData);
    const auto replace = [data](const std::string& text) {
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, text.c_str());
    };
    switch (data->EventFlag) {
    case ImGuiInputTextFlags_CallbackCharFilter:
        // The toggle key types a ` as well; it must not end up in the line.
        return data->EventChar == '`' ? 1 : 0;
    case ImGuiInputTextFlags_CallbackHistory: {
        const auto entry = data->EventKey == ImGuiKey_UpArrow ? console->historyUp() : console->historyDown();
        if (entry) {
            replace(*entry);
        }
        return 0;
    }
    case ImGuiInputTextFlags_CallbackCompletion:
        replace(console->completeLine(std::string_view(data->Buf, static_cast<usize>(data->BufTextLen))));
        return 0;
    default:
        return 0;
    }
}

void DeveloperConsole::draw() {
    if (!m_open) {
        return;
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({8.0F, display.y * 0.5F}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({std::min(display.x - 16.0F, 900.0F), display.y * 0.45F},
                             ImGuiCond_FirstUseEver);
    bool open = true;
    // Only the scrollback scrolls; the window around it stays put.
    if (ImGui::Begin("Console", &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        const f32 footer = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
        if (ImGui::BeginChild("Scrollback", {0.0F, -footer}, ImGuiChildFlags_None,
                              ImGuiWindowFlags_HorizontalScrollbar)) {
            for (const Line& line : m_lines) {
                ImGui::PushStyleColor(ImGuiCol_Text, lineColor(line.kind));
                ImGui::TextUnformatted(line.text.c_str());
                ImGui::PopStyleColor();
            }
            if (m_scrollToBottom) {
                ImGui::SetScrollHereY(1.0F);
                m_scrollToBottom = false;
            }
        }
        ImGui::EndChild();
        ImGui::Separator();
        constexpr ImGuiInputTextFlags kFlags =
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCharFilter |
            ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackCompletion;
        ImGui::SetNextItemWidth(-1.0F);
        if (m_focusInput) {
            ImGui::SetKeyboardFocusHere();
            m_focusInput = false;
        }
        if (ImGui::InputText("##command", m_inputBuffer.data(), m_inputBuffer.size(), kFlags, &inputCallback,
                             this)) {
            static_cast<void>(execute(m_inputBuffer.data()));
            m_inputBuffer.fill('\0');
            m_focusInput = true; // Keep typing after Enter.
        }
    }
    ImGui::End();
    if (!open) {
        setOpen(false);
    }
}

} // namespace vxe::debug
