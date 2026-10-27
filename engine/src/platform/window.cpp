#include "vxe/platform/window.hpp"

#include <vxe/core/log.hpp>
#include <vxe/platform/platform.hpp>

#include <SDL3/SDL.h>

#include <array>
#include <format>
#include <string>

namespace vxe::platform {
namespace {

Error sdlError(std::string_view what) {
    return Error(ErrorCode::Unsupported, std::format("{}: {}", what, SDL_GetError()));
}

// Asks for an OpenGL core context of the given version. Must run before the
// window is created, because the attributes choose its pixel format.
void requestGlContext(int major, int minor, bool debug) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, debug ? SDL_GL_CONTEXT_DEBUG_FLAG : 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    // The renderer draws into its own targets with a 32 bit float depth buffer
    // (ADR-010), so the window's framebuffer needs no depth or stencil.
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
}

} // namespace

Result<std::unique_ptr<Window>> Window::create(Platform& platform, const WindowDesc& desc) {
    if (!platform.hasVideo()) {
        return Error(ErrorCode::Unsupported, "The platform was started without video");
    }

    SDL_WindowFlags flags = 0;
    if (desc.api == GraphicsApi::OpenGL) {
        flags |= SDL_WINDOW_OPENGL;
    }
    if (desc.resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    if (desc.highDpi) {
        flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
    }
    if (desc.hidden) {
        flags |= SDL_WINDOW_HIDDEN;
    }

    std::unique_ptr<Window> window(new Window());

    if (desc.api == GraphicsApi::OpenGL) {
        // OpenGL 4.6 is preferred, 4.5 is the minimum (Section 4): try them in order.
        constexpr std::array<std::array<int, 2>, 2> kVersions{{{4, 6}, {4, 5}}};
        for (const auto& [major, minor] : kVersions) {
            requestGlContext(major, minor, desc.debugContext);
            window->m_window = SDL_CreateWindow(desc.title.c_str(), desc.width, desc.height, flags);
            if (window->m_window == nullptr) {
                return sdlError("Cannot create the window");
            }
            window->m_glContext = SDL_GL_CreateContext(window->m_window);
            if (window->m_glContext != nullptr) {
                window->m_glInfo =
                    GlContextInfo{.major = major, .minor = minor, .core = true, .debug = desc.debugContext};
                break;
            }
            VXE_LOG_WARN(log::core, "OpenGL {}.{} core context not available: {}", major, minor,
                         SDL_GetError());
            SDL_DestroyWindow(window->m_window);
            window->m_window = nullptr;
        }
        if (window->m_glContext == nullptr) {
            return Error(
                ErrorCode::Unsupported,
                "This GPU or driver does not support OpenGL 4.5 or newer, which Voxel Rethinked requires");
        }
    } else {
        window->m_window = SDL_CreateWindow(desc.title.c_str(), desc.width, desc.height, flags);
        if (window->m_window == nullptr) {
            return sdlError("Cannot create the window");
        }
    }

    if (desc.mode != WindowMode::Windowed) {
        Result<void> mode = window->setMode(desc.mode);
        if (!mode) {
            return std::move(mode).error();
        }
    }
    VXE_LOG_INFO(log::core, "Window {}x{} (pixels {}x{}, scale {:.2f}){}", window->size().x, window->size().y,
                 window->pixelSize().x, window->pixelSize().y, window->displayScale(),
                 window->m_glContext != nullptr
                     ? std::format(", OpenGL {}.{} core{}", window->m_glInfo.major, window->m_glInfo.minor,
                                   window->m_glInfo.debug ? " debug" : "")
                     : std::string());
    return window;
}

Window::~Window() {
    if (m_glContext != nullptr) {
        SDL_GL_DestroyContext(m_glContext);
    }
    if (m_window != nullptr) {
        SDL_DestroyWindow(m_window);
    }
}

Result<void> Window::setMode(WindowMode mode) {
    switch (mode) {
    case WindowMode::Windowed:
        if (!SDL_SetWindowFullscreen(m_window, false)) {
            return sdlError("Cannot leave fullscreen");
        }
        break;
    case WindowMode::Borderless:
        // A null mode means "fullscreen at the desktop resolution".
        if (!SDL_SetWindowFullscreenMode(m_window, nullptr) || !SDL_SetWindowFullscreen(m_window, true)) {
            return sdlError("Cannot switch to borderless fullscreen");
        }
        break;
    case WindowMode::Fullscreen: {
        const SDL_DisplayID display = SDL_GetDisplayForWindow(m_window);
        const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display);
        if (desktop == nullptr || !SDL_SetWindowFullscreenMode(m_window, desktop) ||
            !SDL_SetWindowFullscreen(m_window, true)) {
            return sdlError("Cannot switch to exclusive fullscreen");
        }
        break;
    }
    }
    // Mode changes are asynchronous on some platforms; wait for this one so
    // size queries right after see the new state.
    SDL_SyncWindow(m_window);
    m_mode = mode;
    return {};
}

void Window::setTitle(std::string_view title) {
    const std::string copy(title);
    SDL_SetWindowTitle(m_window, copy.c_str());
}

void Window::setSize(i32 width, i32 height) {
    SDL_SetWindowSize(m_window, width, height);
    SDL_SyncWindow(m_window);
}

void Window::show() {
    SDL_ShowWindow(m_window);
}

IVec2 Window::size() const {
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(m_window, &width, &height);
    return {width, height};
}

IVec2 Window::pixelSize() const {
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(m_window, &width, &height);
    return {width, height};
}

f32 Window::displayScale() const {
    const float scale = SDL_GetWindowDisplayScale(m_window);
    return scale > 0.0F ? scale : 1.0F;
}

bool Window::isFocused() const {
    return (SDL_GetWindowFlags(m_window) & SDL_WINDOW_INPUT_FOCUS) != 0;
}

bool Window::isMinimized() const {
    return (SDL_GetWindowFlags(m_window) & SDL_WINDOW_MINIMIZED) != 0;
}

Result<void> Window::setRelativeMouseMode(bool enabled) {
    if (!SDL_SetWindowRelativeMouseMode(m_window, enabled)) {
        return sdlError("Cannot change relative mouse mode");
    }
    return {};
}

bool Window::isRelativeMouseMode() const {
    return SDL_GetWindowRelativeMouseMode(m_window);
}

void* Window::glGetProcAddress(const char* name) noexcept {
    // SDL returns a generic function pointer. Loaders such as glad take it as
    // void*, which every platform the engine targets supports.
    return reinterpret_cast<void*>(SDL_GL_GetProcAddress(name));
}

void Window::swapGlBuffers() {
    SDL_GL_SwapWindow(m_window);
}

// A member on purpose: the interval applies to this window's current context.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
bool Window::setGlSwapInterval(int interval) {
    return SDL_GL_SetSwapInterval(interval);
}

} // namespace vxe::platform
