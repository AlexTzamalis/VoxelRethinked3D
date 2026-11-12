#include "vxe/platform/platform.hpp"

#include <vxe/core/log.hpp>
#include <vxe/core/profile.hpp>
#include <vxe/platform/window.hpp>

#include <SDL3/SDL.h>
// Only for SDL_SetMainReady. SDL_MAIN_HANDLED stops the header from replacing
// main(), because the engine provides its own entry point.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <mutex>

namespace vxe::platform {
namespace {

// SDL is process global, so a second Platform would initialize it twice and
// tear it down under the first one.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): guards the single SDL instance.
std::atomic<bool> platformExists{false};

Modifiers toModifiers(SDL_Keymod mod) noexcept {
    Modifiers result = Modifiers::None;
    if ((mod & SDL_KMOD_SHIFT) != 0) {
        result = result | Modifiers::Shift;
    }
    if ((mod & SDL_KMOD_CTRL) != 0) {
        result = result | Modifiers::Ctrl;
    }
    if ((mod & SDL_KMOD_ALT) != 0) {
        result = result | Modifiers::Alt;
    }
    if ((mod & SDL_KMOD_GUI) != 0) {
        result = result | Modifiers::Gui;
    }
    return result;
}

// Returns false for SDL events the engine does not use.
bool translate(const SDL_Event& event, std::vector<PlatformEvent>& out) {
    switch (event.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        out.emplace_back(QuitRequested{});
        return true;
    case SDL_EVENT_WINDOW_RESIZED:
        out.emplace_back(WindowResized{.width = event.window.data1, .height = event.window.data2});
        return true;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        out.emplace_back(WindowPixelSizeChanged{.width = event.window.data1, .height = event.window.data2});
        return true;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        out.emplace_back(WindowFocusChanged{.focused = event.type == SDL_EVENT_WINDOW_FOCUS_GAINED});
        return true;
    case SDL_EVENT_WINDOW_MINIMIZED:
    case SDL_EVENT_WINDOW_RESTORED:
        out.emplace_back(WindowMinimizedChanged{.minimized = event.type == SDL_EVENT_WINDOW_MINIMIZED});
        return true;
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: {
        SDL_Window* window = SDL_GetWindowFromID(event.window.windowID);
        out.emplace_back(
            DisplayScaleChanged{.scale = window != nullptr ? SDL_GetWindowDisplayScale(window) : 1.0F});
        return true;
    }
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        out.emplace_back(KeyEvent{.scancode = static_cast<u16>(event.key.scancode),
                                  .down = event.type == SDL_EVENT_KEY_DOWN,
                                  .repeat = event.key.repeat,
                                  .modifiers = toModifiers(event.key.mod)});
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        out.emplace_back(MouseButtonEvent{.button = static_cast<MouseButton>(event.button.button),
                                          .down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN,
                                          .clicks = event.button.clicks,
                                          .x = event.button.x,
                                          .y = event.button.y});
        return true;
    case SDL_EVENT_MOUSE_MOTION:
        out.emplace_back(MouseMotionEvent{
            .x = event.motion.x, .y = event.motion.y, .dx = event.motion.xrel, .dy = event.motion.yrel});
        return true;
    case SDL_EVENT_MOUSE_WHEEL: {
        // Some systems report "natural" scrolling as flipped; normalize so
        // positive y always means away from the user.
        const f32 sign = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0F : 1.0F;
        out.emplace_back(MouseWheelEvent{.x = event.wheel.x * sign, .y = event.wheel.y * sign});
        return true;
    }
    case SDL_EVENT_TEXT_INPUT:
        out.emplace_back(TextInputEvent{.text = event.text.text != nullptr ? event.text.text : ""});
        return true;
    default:
        return false;
    }
}

} // namespace

Result<std::unique_ptr<Platform>> Platform::create(const PlatformConfig& config) {
    if (platformExists.exchange(true)) {
        return Error(ErrorCode::AlreadyExists, "Only one Platform may exist at a time");
    }
    // We provide our own main(), so tell SDL that the main entry point is ready.
    SDL_SetMainReady();
    const SDL_InitFlags flags = SDL_INIT_EVENTS | (config.video ? SDL_INIT_VIDEO : 0U);
    if (!SDL_Init(flags)) {
        platformExists.store(false);
        return Error(ErrorCode::Unsupported, std::format("SDL could not start: {}", SDL_GetError()));
    }
    VXE_LOG_INFO(log::core, "Platform started (SDL {}.{}.{}, video {})", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
                 SDL_MICRO_VERSION, config.video ? "on" : "off");
    return std::unique_ptr<Platform>(new Platform(config.video));
}

namespace detail {

// What a picker hands back from its own thread: the request number and the
// chosen file. Shared with every open picker, so a picker that answers after
// the Platform is gone writes into memory that still exists.
struct FileDialogResults {
    std::mutex mutex;
    std::vector<std::pair<u64, std::optional<std::filesystem::path>>> done;
};

} // namespace detail

namespace {

// One open picker: SDL wants the filter strings to live until it answers.
struct FileDialogRequest {
    std::shared_ptr<detail::FileDialogResults> results;
    u64 id = 0;
    std::vector<std::string> names;
    std::vector<std::string> patterns;
    std::vector<SDL_DialogFileFilter> filters;
    std::string location;
};

void SDLCALL fileDialogAnswered(void* userdata, const char* const* files, int /*filter*/) {
    std::unique_ptr<FileDialogRequest> request(static_cast<FileDialogRequest*>(userdata));
    std::optional<std::filesystem::path> chosen;
    if (files != nullptr && files[0] != nullptr) {
        const std::string_view text(files[0]);
        chosen = std::filesystem::path(std::u8string(text.begin(), text.end()));
    }
    const std::scoped_lock lock(request->results->mutex);
    request->results->done.emplace_back(request->id, std::move(chosen));
}

} // namespace

Platform::Platform(bool video)
    : m_video(video)
    , m_dialogResults(std::make_shared<detail::FileDialogResults>()) {}

Platform::~Platform() {
    SDL_Quit();
    platformExists.store(false);
}

void Platform::showFileDialog(Window* parent, FileDialogDesc desc, FileDialogDone done) {
    const u64 id = m_nextDialog++;
    m_dialogsOpen.emplace_back(id, std::move(done));
    if (!m_video) {
        const std::scoped_lock lock(m_dialogResults->mutex);
        m_dialogResults->done.emplace_back(id, std::nullopt);
        return;
    }
    auto request = std::make_unique<FileDialogRequest>();
    request->results = m_dialogResults;
    request->id = id;
    for (FileDialogFilter& filter : desc.filters) {
        request->names.push_back(std::move(filter.name));
        request->patterns.push_back(std::move(filter.pattern));
    }
    for (usize i = 0; i < request->names.size(); ++i) {
        request->filters.push_back(
            {.name = request->names[i].c_str(), .pattern = request->patterns[i].c_str()});
    }
    const std::u8string location = desc.defaultLocation.u8string();
    request->location.assign(location.begin(), location.end());
    SDL_Window* window = parent != nullptr ? parent->nativeHandle() : nullptr;
    const char* where = request->location.empty() ? nullptr : request->location.c_str();
    const SDL_DialogFileFilter* filters = request->filters.empty() ? nullptr : request->filters.data();
    const int count = static_cast<int>(request->filters.size());
    // SDL owns the request until it answers (fileDialogAnswered deletes it).
    FileDialogRequest* owned = request.release();
    if (desc.save) {
        SDL_ShowSaveFileDialog(fileDialogAnswered, owned, window, filters, count, where);
    } else {
        SDL_ShowOpenFileDialog(fileDialogAnswered, owned, window, filters, count, where, false);
    }
}

void Platform::deliverFileDialogs() {
    std::vector<std::pair<u64, std::optional<std::filesystem::path>>> done;
    {
        const std::scoped_lock lock(m_dialogResults->mutex);
        done.swap(m_dialogResults->done);
    }
    for (auto& [id, chosen] : done) {
        const auto open = std::ranges::find(m_dialogsOpen, id, &std::pair<u64, FileDialogDone>::first);
        if (open == m_dialogsOpen.end()) {
            continue;
        }
        FileDialogDone callback = std::move(open->second);
        m_dialogsOpen.erase(open);
        if (callback) {
            callback(std::move(chosen));
        }
    }
}

// A member on purpose: it needs SDL started, which a live Platform guarantees.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
usize Platform::pollEvents(std::vector<PlatformEvent>& out) {
    VXE_PROFILE_FUNCTION();
    usize count = m_injected.size();
    out.insert(out.end(), m_injected.begin(), m_injected.end());
    m_injected.clear();
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (translate(event, out)) {
            ++count;
        }
    }
    if (!m_dialogsOpen.empty()) {
        deliverFileDialogs();
    }
    return count;
}

// A member on purpose: layout names need SDL started, which a live Platform
// guarantees. layoutKeyName is the static form for code without a Platform.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
std::string Platform::keyName(u16 scancode) const {
    return layoutKeyName(scancode);
}

std::string Platform::layoutKeyName(u16 scancode) {
    const SDL_Keycode key = SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(scancode), SDL_KMOD_NONE, false);
    const char* name = SDL_GetKeyName(key);
    if (name != nullptr && name[0] != '\0') {
        return name;
    }
    // Keys without a layout name (rare media keys) fall back to their scancode name.
    const char* scancodeName = SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode));
    return scancodeName != nullptr ? scancodeName : std::string();
}

std::string Platform::scancodeName(u16 scancode) {
    const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode));
    return name != nullptr ? std::string(name) : std::string();
}

u16 Platform::scancodeFromName(std::string_view name) {
    const std::string copy(name);
    const SDL_Scancode code = SDL_GetScancodeFromName(copy.c_str());
    return code == SDL_SCANCODE_UNKNOWN ? u16{0} : static_cast<u16>(code);
}

u16 Platform::scancodeFromKeyName(std::string_view keyName) {
    const std::string copy(keyName);
    const SDL_Keycode key = SDL_GetKeyFromName(copy.c_str());
    if (key == SDLK_UNKNOWN) {
        return 0;
    }
    SDL_Keymod modifiers = SDL_KMOD_NONE;
    const SDL_Scancode code = SDL_GetScancodeFromKey(key, &modifiers);
    return code == SDL_SCANCODE_UNKNOWN ? u16{0} : static_cast<u16>(code);
}

std::filesystem::path Platform::executableFolder() {
    // SDL caches the string and owns it; it must not be freed.
    const char* base = SDL_GetBasePath();
    return base != nullptr ? std::filesystem::path(base) : std::filesystem::current_path();
}

std::filesystem::path Platform::roamingDataFolder() {
#ifdef _WIN32
    const char* folder = SDL_getenv("APPDATA");
#else
    const char* folder = SDL_getenv("HOME");
#endif
    if (folder == nullptr || *folder == '\0') {
        return {};
    }
    // The variable holds UTF-8 (SDL converts it), so the path is built from UTF-8.
    const std::string_view text(folder);
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::filesystem::path Platform::downloadsFolder() {
    const char* folder = SDL_GetUserFolder(SDL_FOLDER_DOWNLOADS); // Owned by SDL.
    if (folder == nullptr) {
        return {};
    }
    const std::string_view text(folder);
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string Platform::fileUrl(const std::filesystem::path& absolutePath) {
    const std::u8string utf8 = absolutePath.generic_u8string();
    std::string url = "file://";
    // Windows paths start with a drive letter, which a URL puts after a third slash.
    if (utf8.empty() || utf8.front() != u8'/') {
        url += '/';
    }
    constexpr std::string_view kHex = "0123456789ABCDEF";
    for (const char8_t unit : utf8) {
        const auto byte = static_cast<unsigned char>(unit);
        const bool plain = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                           (byte >= '0' && byte <= '9') || byte == '/' || byte == ':' || byte == '-' ||
                           byte == '_' || byte == '.' || byte == '~';
        if (plain) {
            url += static_cast<char>(byte);
        } else {
            url += '%';
            url += kHex[byte >> 4U];
            url += kHex[byte & 15U];
        }
    }
    return url;
}

Result<void> Platform::openInFileBrowser(const std::filesystem::path& folder) {
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(folder, error);
    if (error || !std::filesystem::is_directory(absolute, error)) {
        return Error(ErrorCode::NotFound, std::format("No folder at '{}'", folder.string()));
    }
    if (!SDL_OpenURL(fileUrl(absolute).c_str())) {
        return Error(ErrorCode::Unknown,
                     std::format("Could not open '{}': {}", absolute.string(), SDL_GetError()));
    }
    return {};
}

} // namespace vxe::platform
