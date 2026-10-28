#include "vxe/debug/debug_ui.hpp"

#include "vxe/debug/imgui_renderer.hpp"

#include <implot.h>

#include <algorithm>
#include <variant>

namespace vxe::debug {
namespace {

// Scancodes are USB HID usage ids (platform/events.hpp). The ranges below are
// contiguous in both the HID table and the ImGuiKey enum.
constexpr u16 kScancodeA = 4;
constexpr u16 kScancodeZ = 29;
constexpr u16 kScancode1 = 30;
constexpr u16 kScancode9 = 38;
constexpr u16 kScancode0 = 39;
constexpr u16 kScancodeF1 = 58;
constexpr u16 kScancodeF12 = 69;
constexpr u16 kScancodeF13 = 104;
constexpr u16 kScancodeF24 = 115;
constexpr u16 kScancodeKeypad1 = 89;
constexpr u16 kScancodeKeypad9 = 97;

ImGuiKey offsetKey(ImGuiKey first, u16 scancode, u16 firstScancode) noexcept {
    return static_cast<ImGuiKey>(static_cast<int>(first) + (scancode - firstScancode));
}

ImGuiKey singleKey(u16 scancode) noexcept {
    switch (scancode) {
    case 40:
        return ImGuiKey_Enter;
    case 41:
        return ImGuiKey_Escape;
    case 42:
        return ImGuiKey_Backspace;
    case 43:
        return ImGuiKey_Tab;
    case 44:
        return ImGuiKey_Space;
    case 45:
        return ImGuiKey_Minus;
    case 46:
        return ImGuiKey_Equal;
    case 47:
        return ImGuiKey_LeftBracket;
    case 48:
        return ImGuiKey_RightBracket;
    case 49:
        return ImGuiKey_Backslash;
    case 51:
        return ImGuiKey_Semicolon;
    case 52:
        return ImGuiKey_Apostrophe;
    case 53:
        return ImGuiKey_GraveAccent;
    case 54:
        return ImGuiKey_Comma;
    case 55:
        return ImGuiKey_Period;
    case 56:
        return ImGuiKey_Slash;
    case 57:
        return ImGuiKey_CapsLock;
    case 70:
        return ImGuiKey_PrintScreen;
    case 71:
        return ImGuiKey_ScrollLock;
    case 72:
        return ImGuiKey_Pause;
    case 73:
        return ImGuiKey_Insert;
    case 74:
        return ImGuiKey_Home;
    case 75:
        return ImGuiKey_PageUp;
    case 76:
        return ImGuiKey_Delete;
    case 77:
        return ImGuiKey_End;
    case 78:
        return ImGuiKey_PageDown;
    case 79:
        return ImGuiKey_RightArrow;
    case 80:
        return ImGuiKey_LeftArrow;
    case 81:
        return ImGuiKey_DownArrow;
    case 82:
        return ImGuiKey_UpArrow;
    case 83:
        return ImGuiKey_NumLock;
    case 84:
        return ImGuiKey_KeypadDivide;
    case 85:
        return ImGuiKey_KeypadMultiply;
    case 86:
        return ImGuiKey_KeypadSubtract;
    case 87:
        return ImGuiKey_KeypadAdd;
    case 88:
        return ImGuiKey_KeypadEnter;
    case 98:
        return ImGuiKey_Keypad0;
    case 99:
        return ImGuiKey_KeypadDecimal;
    case 101:
        return ImGuiKey_Menu;
    case 103:
        return ImGuiKey_KeypadEqual;
    case 224:
        return ImGuiKey_LeftCtrl;
    case 225:
        return ImGuiKey_LeftShift;
    case 226:
        return ImGuiKey_LeftAlt;
    case 227:
        return ImGuiKey_LeftSuper;
    case 228:
        return ImGuiKey_RightCtrl;
    case 229:
        return ImGuiKey_RightShift;
    case 230:
        return ImGuiKey_RightAlt;
    case 231:
        return ImGuiKey_RightSuper;
    default:
        return ImGuiKey_None;
    }
}

int imguiMouseButton(platform::MouseButton button) noexcept {
    switch (button) {
    case platform::MouseButton::Left:
        return ImGuiMouseButton_Left;
    case platform::MouseButton::Right:
        return ImGuiMouseButton_Right;
    case platform::MouseButton::Middle:
        return ImGuiMouseButton_Middle;
    case platform::MouseButton::X1:
        return 3;
    case platform::MouseButton::X2:
        return 4;
    }
    return -1;
}

} // namespace

ImGuiKey imguiKeyFromScancode(u16 scancode) noexcept {
    if (scancode >= kScancodeA && scancode <= kScancodeZ) {
        return offsetKey(ImGuiKey_A, scancode, kScancodeA);
    }
    if (scancode >= kScancode1 && scancode <= kScancode9) {
        return offsetKey(ImGuiKey_1, scancode, kScancode1);
    }
    if (scancode == kScancode0) {
        return ImGuiKey_0;
    }
    if (scancode >= kScancodeF1 && scancode <= kScancodeF12) {
        return offsetKey(ImGuiKey_F1, scancode, kScancodeF1);
    }
    if (scancode >= kScancodeF13 && scancode <= kScancodeF24) {
        return offsetKey(ImGuiKey_F13, scancode, kScancodeF13);
    }
    if (scancode >= kScancodeKeypad1 && scancode <= kScancodeKeypad9) {
        return offsetKey(ImGuiKey_Keypad1, scancode, kScancodeKeypad1);
    }
    return singleKey(scancode);
}

DebugUi::DebugUi(rhi::Device& device, input::InputSystem& input)
    : m_device(device)
    , m_input(input) {}

Result<std::unique_ptr<DebugUi>> DebugUi::create(rhi::Device& device, input::InputSystem& input,
                                                 const DebugUiDesc& desc) {
    std::unique_ptr<DebugUi> ui(new DebugUi(device, input));
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ui->m_imgui = ImGui::CreateContext();
    ImGui::SetCurrentContext(ui->m_imgui);
    ui->m_implot = ImPlot::CreateContext();
    ImPlot::SetCurrentContext(ui->m_implot);

    ImGuiIO& io = ImGui::GetIO();
    io.BackendPlatformName = "vxe_platform";
    io.BackendRendererName = "vxe_rhi";
    // Vertex offsets: large UIs need more than 65536 vertices per list with
    // 16 bit indices. Textures: ImGui hands texture work to the renderer.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
    // Keyboard navigation stays off: with it, ImGui claims the keyboard
    // whenever any debug window has focus, so gameplay would lose WASD for as
    // long as the overlay is open. Without it the keyboard is only taken
    // while a widget is active or a text field is typing.
    if (desc.docking) {
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    }
    // No imgui.ini in the working directory. Window layouts get a proper
    // home (config:/) with the overlay in E7.3.
    io.IniFilename = nullptr;
    ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
    platformIo.Renderer_TextureMaxWidth = static_cast<int>(device.capabilities().maxTextureSize);
    platformIo.Renderer_TextureMaxHeight = static_cast<int>(device.capabilities().maxTextureSize);

    auto renderer = ImGuiRenderer::create(device, desc.shaders, desc.targetFormat);
    if (!renderer) {
        ImGui::SetCurrentContext(previous);
        return renderer.error();
    }
    ui->m_renderer = std::move(renderer.value());
    ui->m_context = input.registerContext({.name = "Debug UI", .blocksLowerContexts = true});
    ImGui::SetCurrentContext(previous);
    return ui;
}

DebugUi::~DebugUi() {
    if (m_imgui == nullptr) {
        return;
    }
    ImGuiContext* previous = ImGui::GetCurrentContext();
    makeCurrent();
    if (m_inFrame) {
        ImGui::EndFrame();
    }
    if (m_captured) {
        m_input.popContext(m_context);
    }
    if (m_renderer) {
        m_renderer->releaseTextures();
        m_renderer.reset();
    }
    ImPlot::DestroyContext(m_implot);
    ImGui::DestroyContext(m_imgui);
    ImGui::SetCurrentContext(previous == m_imgui ? nullptr : previous);
}

void DebugUi::makeCurrent() const {
    ImGui::SetCurrentContext(m_imgui);
    ImPlot::SetCurrentContext(m_implot);
}

void DebugUi::processEvents(std::span<const platform::PlatformEvent> events) {
    for (const platform::PlatformEvent& event : events) {
        processEvent(event);
    }
}

void DebugUi::processEvent(const platform::PlatformEvent& event) {
    makeCurrent();
    ImGuiIO& io = ImGui::GetIO();
    if (const auto* key = std::get_if<platform::KeyEvent>(&event)) {
        // Modifier state first, so shortcuts like Ctrl+C see it.
        io.AddKeyEvent(ImGuiMod_Ctrl, platform::hasAll(key->modifiers, platform::Modifiers::Ctrl));
        io.AddKeyEvent(ImGuiMod_Shift, platform::hasAll(key->modifiers, platform::Modifiers::Shift));
        io.AddKeyEvent(ImGuiMod_Alt, platform::hasAll(key->modifiers, platform::Modifiers::Alt));
        io.AddKeyEvent(ImGuiMod_Super, platform::hasAll(key->modifiers, platform::Modifiers::Gui));
        if (const ImGuiKey imguiKey = imguiKeyFromScancode(key->scancode); imguiKey != ImGuiKey_None) {
            io.AddKeyEvent(imguiKey, key->down);
        }
    } else if (const auto* motion = std::get_if<platform::MouseMotionEvent>(&event)) {
        io.AddMousePosEvent(motion->x, motion->y);
    } else if (const auto* button = std::get_if<platform::MouseButtonEvent>(&event)) {
        io.AddMousePosEvent(button->x, button->y);
        if (const int index = imguiMouseButton(button->button); index >= 0) {
            io.AddMouseButtonEvent(index, button->down);
        }
    } else if (const auto* wheel = std::get_if<platform::MouseWheelEvent>(&event)) {
        io.AddMouseWheelEvent(wheel->x, wheel->y);
    } else if (const auto* text = std::get_if<platform::TextInputEvent>(&event)) {
        io.AddInputCharactersUTF8(text->text.c_str());
    } else if (const auto* focus = std::get_if<platform::WindowFocusChanged>(&event)) {
        io.AddFocusEvent(focus->focused);
    }
}

void DebugUi::beginFrame(f64 deltaSeconds, Vec2 displaySize, Vec2 framebufferSize) {
    makeCurrent();
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = deltaSeconds > 0.0 ? static_cast<f32>(deltaSeconds) : 1.0F / 60.0F;
    io.DisplaySize = ImVec2(std::max(displaySize.x, 1.0F), std::max(displaySize.y, 1.0F));
    io.DisplayFramebufferScale =
        ImVec2(framebufferSize.x / io.DisplaySize.x, framebufferSize.y / io.DisplaySize.y);
    ImGui::NewFrame();
    m_inFrame = true;
}

void DebugUi::endFrame() {
    makeCurrent();
    ImGui::Render();
    m_inFrame = false;
    m_renderer->updateTextures(*ImGui::GetDrawData());

    // Input routing: the UI's context blocks gameplay while ImGui uses input.
    // ImGui decides this from the frame that just ended, so the switch takes
    // effect one frame later, which matches how ImGui itself reports it.
    const ImGuiIO& io = ImGui::GetIO();
    const bool capture = io.WantCaptureMouse || io.WantCaptureKeyboard;
    if (capture != m_captured) {
        if (capture) {
            m_input.pushContext(m_context);
        } else {
            m_input.popContext(m_context);
        }
        m_captured = capture;
    }
}

void DebugUi::record(rhi::CommandList& commands) {
    makeCurrent();
    if (const ImDrawData* drawData = ImGui::GetDrawData(); drawData != nullptr && drawData->Valid) {
        m_renderer->record(commands, *drawData);
    }
}

bool DebugUi::wantsTextInput() const {
    makeCurrent();
    return ImGui::GetIO().WantTextInput;
}

} // namespace vxe::debug
