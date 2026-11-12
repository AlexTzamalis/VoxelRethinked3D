#pragma once

#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>
#include <vxe/platform/events.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vxe::platform {

class Window;

// One choice in a file picker's type list, like {"World", "zip"}. `pattern`
// lists extensions without dots, separated by semicolons ("png;jpg"), or "*".
struct FileDialogFilter {
    std::string name;
    std::string pattern;
};

struct FileDialogDesc {
    bool save = false; // A "save as" picker; else one to open an existing file.
    std::vector<FileDialogFilter> filters;
    std::filesystem::path defaultLocation; // A folder, or a file name for save pickers.
};

// Called with the chosen file, or nothing when the player cancelled or the
// system has no picker.
using FileDialogDone = std::function<void(std::optional<std::filesystem::path>)>;

namespace detail {
struct FileDialogResults;
} // namespace detail

struct PlatformConfig {
    // Start the video subsystem (windows, displays). Off for headless tools,
    // tests and a future dedicated server, which still get events and timers.
    bool video = true;
};

// Owns the platform layer (SDL3, ADR-004) for the lifetime of the program.
//
// Exactly one Platform may exist at a time. Windows are created through it
// and must be destroyed before it. Everything SDL related happens on the main
// thread, which is also why only the main thread may call into this class.
class Platform {
public:
    [[nodiscard]] static Result<std::unique_ptr<Platform>> create(const PlatformConfig& config = {});
    ~Platform();

    Platform(const Platform&) = delete;
    Platform& operator=(const Platform&) = delete;
    Platform(Platform&&) = delete;
    Platform& operator=(Platform&&) = delete;

    // Translates every pending OS event and appends it to `out`. Call once per
    // frame on the main thread. Returns the number of events appended.
    usize pollEvents(std::vector<PlatformEvent>& out);
    // Delivers an event with the next pollEvents, as if the system had sent
    // it: scripted runs drive the real input path this way.
    void injectEvent(const PlatformEvent& event) { m_injected.push_back(event); }

    // The label of a key for the keybind screen, in the current keyboard
    // layout (for example "Z" for the scancode labeled Y on a German keyboard).
    [[nodiscard]] std::string keyName(u16 scancode) const;

    // Same as keyName, usable without a Platform object (keybind files).
    [[nodiscard]] static std::string layoutKeyName(u16 scancode);

    // Layout independent name of a physical key, for files ("W", "Space",
    // "Left Shift"). Works without a Platform. Empty for unknown scancodes.
    [[nodiscard]] static std::string scancodeName(u16 scancode);
    // The inverse of scancodeName. 0 if the name is unknown.
    [[nodiscard]] static u16 scancodeFromName(std::string_view name);
    // The scancode that types the key labeled `keyName` in the current
    // layout ("Z" on a German keyboard is the physical Y key). 0 if unknown.
    // Needs the platform started with video for layout information.
    [[nodiscard]] static u16 scancodeFromKeyName(std::string_view keyName);

    // The folder of the running executable. Runtime folders such as config/
    // and textures/ live next to it (Section 5).
    [[nodiscard]] static std::filesystem::path executableFolder();

    // Where programs keep their users' files: %APPDATA% on Windows, the home
    // folder elsewhere. Empty when the system does not say. Works without a
    // Platform.
    [[nodiscard]] static std::filesystem::path roamingDataFolder();
    // The user's Downloads folder, or empty when the system has none.
    [[nodiscard]] static std::filesystem::path downloadsFolder();

    // Shows a folder in the system's file browser (Explorer on Windows), for
    // buttons like "Open textures folder". Fails if the folder does not exist
    // or the system refuses.
    [[nodiscard]] static Result<void> openInFileBrowser(const std::filesystem::path& folder);
    // A file:// URL of an absolute path, with every byte that is not plainly
    // safe in a URL written as %XX (spaces, non English letters).
    [[nodiscard]] static std::string fileUrl(const std::filesystem::path& absolutePath);

    [[nodiscard]] bool hasVideo() const noexcept { return m_video; }

    // Opens the system's file picker over `parent` (may be null) without
    // waiting for it. `done` runs on the main thread inside a later
    // pollEvents. Without video, `done` gets nothing in the next pollEvents.
    void showFileDialog(Window* parent, FileDialogDesc desc, FileDialogDone done);

private:
    explicit Platform(bool video);
    void deliverFileDialogs();

    bool m_video = false;
    std::vector<PlatformEvent> m_injected;
    // Filled by the picker's thread, emptied on the main thread.
    std::shared_ptr<detail::FileDialogResults> m_dialogResults;
    std::vector<std::pair<u64, FileDialogDone>> m_dialogsOpen;
    u64 m_nextDialog = 1;
};

} // namespace vxe::platform
