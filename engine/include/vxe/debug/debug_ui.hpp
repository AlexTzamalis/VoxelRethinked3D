#pragma once

#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>
#include <vxe/input/input.hpp>
#include <vxe/math/vector.hpp>
#include <vxe/platform/events.hpp>
#include <vxe/rhi/command_list.hpp>
#include <vxe/rhi/device.hpp>
#include <vxe/rhi/shader_source.hpp>

#include <imgui.h>

#include <filesystem>
#include <memory>
#include <span>

struct ImPlotContext;

namespace vxe::debug {

class ImGuiRenderer;

// The ImGuiKey for a key scancode (a USB HID usage id, as in
// platform::KeyEvent), or ImGuiKey_None for keys ImGui does not know.
[[nodiscard]] ImGuiKey imguiKeyFromScancode(u16 scancode) noexcept;

struct DebugUiDesc {
    // Reads the engine shader folder (debug/imgui.vert and .frag).
    rhi::ShaderFileReader shaders{};
    // Color format of the pass the UI draws into.
    rhi::Format targetFormat = rhi::Format::RGBA8Unorm;
    // ImGui docking: windows can be docked into each other (Section 18.2:
    // every overlay page is a dockable window).
    bool docking = true;
    // Where window positions, sizes and docking are kept between runs (for
    // example config/imgui_layout.ini). Read once at creation and written once
    // at destruction, never during frames. Empty keeps nothing.
    std::filesystem::path layoutFile{};
};

// Dear ImGui and ImPlot for developer tools (ADR-013, E7.2): owns both
// contexts, feeds them OS input, and draws through the RHI.
//
// Input routing: while ImGui wants the mouse or keyboard (a window is
// hovered, a text field has focus), the UI pushes its own input context,
// registered with blocksLowerContexts, so gameplay actions below it do not
// also react to those clicks and keys. Contexts registered as alwaysActive
// (fullscreen, screenshots) keep working.
//
// Per frame, in this order:
//   processEvents(events)            the frame's OS events
//   beginFrame(dt, sizes)            then any ImGui:: and ImPlot:: calls
//   endFrame()                       finishes the ImGui frame
//   record(commands)                 inside a render pass over the framebuffer
//
// Thread safety: main thread only. Every call makes this UI's ImGui context
// current, so several UIs (for example in tests) do not disturb each other.
class DebugUi {
public:
    [[nodiscard]] static Result<std::unique_ptr<DebugUi>>
    create(rhi::Device& device, input::InputSystem& input, const DebugUiDesc& desc);
    ~DebugUi();
    DebugUi(const DebugUi&) = delete;
    DebugUi& operator=(const DebugUi&) = delete;
    DebugUi(DebugUi&&) = delete;
    DebugUi& operator=(DebugUi&&) = delete;

    void processEvents(std::span<const platform::PlatformEvent> events);
    void processEvent(const platform::PlatformEvent& event);

    // `displaySize` is the window size in logical units (what mouse events
    // use), `framebufferSize` the size in pixels (what is drawn).
    void beginFrame(f64 deltaSeconds, Vec2 displaySize, Vec2 framebufferSize);
    void endFrame();
    void record(rhi::CommandList& commands);

    // True while ImGui takes input, which also means the UI's input context
    // is on the stack.
    [[nodiscard]] bool capturesInput() const noexcept { return m_captured; }
    // True while a text field wants typed text; the window should then have
    // text input enabled (platform::Window::setTextInputEnabled).
    [[nodiscard]] bool wantsTextInput() const;
    [[nodiscard]] input::InputContextId inputContext() const noexcept { return m_context; }

    [[nodiscard]] ImGuiContext* imguiContext() const noexcept { return m_imgui; }
    [[nodiscard]] ImPlotContext* plotContext() const noexcept { return m_implot; }

private:
    // Defined in the .cpp, where ImGuiRenderer is a complete type.
    DebugUi(rhi::Device& device, input::InputSystem& input);

    void makeCurrent() const;

    rhi::Device& m_device;
    input::InputSystem& m_input;
    std::unique_ptr<ImGuiRenderer> m_renderer;
    ImGuiContext* m_imgui = nullptr;
    ImPlotContext* m_implot = nullptr;
    input::InputContextId m_context;
    std::filesystem::path m_layoutFile;
    bool m_captured = false;
    bool m_inFrame = false;
};

} // namespace vxe::debug
