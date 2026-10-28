#pragma once

#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>
#include <vxe/math/vector.hpp>

#include <memory>
#include <string>
#include <string_view>

// Opaque SDL types, so this header does not pull in SDL.
struct SDL_Window;
struct SDL_GLContextState;

namespace vxe::platform {

class Platform;

enum class WindowMode : u8 {
    Windowed,
    Borderless, // Fullscreen at the desktop resolution, no mode change. Fast alt tab.
    Fullscreen, // Exclusive fullscreen at the desktop mode. Can lower latency on some drivers.
};

// Which graphics API the window is created for. The window must know before
// it exists, because an OpenGL window needs a pixel format chosen up front.
enum class GraphicsApi : u8 {
    None,   // No rendering (tools).
    OpenGL, // OpenGL 4.6 core profile (ADR-003).
};

struct WindowDesc {
    std::string title = "Voxel Rethinked";
    i32 width = 1280; // Logical size; the pixel size may be larger on high DPI screens.
    i32 height = 720;
    WindowMode mode = WindowMode::Windowed;
    GraphicsApi api = GraphicsApi::OpenGL;
    bool resizable = true;
    bool highDpi = true;       // Render at full pixel density.
    bool debugContext = false; // OpenGL debug context, for KHR_debug messages (E6.3).
    bool hidden = false;       // Create hidden, for tests that should not flash a window.
};

// The OpenGL context version a window got, for the capability report.
struct GlContextInfo {
    i32 major = 0;
    i32 minor = 0;
    bool core = false;
    bool debug = false;
};

// A window, plus its OpenGL context when created with GraphicsApi::OpenGL.
//
// The context is created here (step E4.4 of the plan) but never used here: no GL
// call happens outside the OpenGL RHI backend. The backend gets the context
// and the function loader through glContext() and glGetProcAddress().
//
// Thread safety: main thread only, like the platform.
class Window {
public:
    [[nodiscard]] static Result<std::unique_ptr<Window>> create(Platform& platform, const WindowDesc& desc);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&) = delete;
    Window& operator=(Window&&) = delete;

    // Switches between windowed, borderless and exclusive fullscreen.
    Result<void> setMode(WindowMode mode);
    [[nodiscard]] WindowMode mode() const noexcept { return m_mode; }

    void setTitle(std::string_view title);
    // Changes the logical size of a windowed window.
    void setSize(i32 width, i32 height);
    void show();

    [[nodiscard]] IVec2 size() const;      // Logical units.
    [[nodiscard]] IVec2 pixelSize() const; // Framebuffer pixels.
    [[nodiscard]] f32 displayScale() const;
    [[nodiscard]] bool isFocused() const;
    [[nodiscard]] bool isMinimized() const;

    // Hides the cursor, keeps it inside the window and reports raw device
    // motion instead of positions (Section 17.1). For first person mouse look.
    Result<void> setRelativeMouseMode(bool enabled);
    [[nodiscard]] bool isRelativeMouseMode() const;

    // SDL only delivers TextInputEvents while text input is on, and on some
    // platforms turning it on shows an on screen keyboard or an input method
    // window. So it is on only while a text field has focus (the debug UI
    // asks for it through DebugUi::wantsTextInput()).
    void setTextInputEnabled(bool enabled);
    [[nodiscard]] bool isTextInputEnabled() const;

    // OpenGL only. These exist for the OpenGL RHI backend.
    [[nodiscard]] SDL_GLContextState* glContext() const noexcept { return m_glContext; }
    [[nodiscard]] GlContextInfo glContextInfo() const noexcept { return m_glInfo; }
    // A function pointer as loaders such as glad expect it. The type matches
    // SDL's and glad's, so no cast between function and object pointers is needed.
    using GlProc = void (*)();
    [[nodiscard]] static GlProc glGetProcAddress(const char* name) noexcept;
    // Presents the back buffer. Called by the OpenGL backend at the end of a frame.
    void swapGlBuffers();
    // 0 for immediate, 1 for vsync, -1 for adaptive vsync. Returns false if
    // the driver refuses the value (adaptive vsync is not always available).
    bool setGlSwapInterval(int interval);

    [[nodiscard]] SDL_Window* nativeHandle() const noexcept { return m_window; }

private:
    Window() = default;

    SDL_Window* m_window = nullptr;
    SDL_GLContextState* m_glContext = nullptr;
    GlContextInfo m_glInfo{};
    WindowMode m_mode = WindowMode::Windowed;
};

} // namespace vxe::platform
