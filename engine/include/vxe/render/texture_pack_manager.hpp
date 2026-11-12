#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/core/result.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/time.hpp>
#include <vxe/core/types.hpp>
#include <vxe/io/file_watcher.hpp>
#include <vxe/jobs/job_system.hpp>
#include <vxe/render/chunk_renderer.hpp>
#include <vxe/render/texture_pack.hpp>
#include <vxe/rhi/device.hpp>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace vxe::render {

struct TexturePackManagerDesc {
    rhi::Device* device = nullptr;
    config::SettingsRegistry* settings = nullptr;
    jobs::JobSystem* jobs = nullptr;
    stats::StatRegistry* stats = nullptr; // Optional.
    std::filesystem::path packsFolder{};  // textures/ next to the executable; may not exist.
    // The player's own packs (resourcepacks/ in the user folder); may be
    // empty. A pack here with the id of a shipped one is skipped.
    std::filesystem::path userPacksFolder{};
    io::FileWatcher* watcher = nullptr; // Optional: hot reload (Graphics.Textures.HotReload).
    // The default pack built into the executable: used for whatever no
    // folder pack has (Section 15.1), so every key always resolves.
    std::unique_ptr<PackSource> builtIn{};
};

// Keeps the block textures of the enabled texture packs on the GPU (Section
// 15.3). Graphics.Textures.Packs lists the enabled pack folders, highest
// priority first ("MyPack, OtherPack"); the "default" folder pack always
// comes last, then the built in pack. Each texture key comes from the first
// pack that has it.
//
// Loading runs on a job (decoding PNGs, scaling, mipmaps); update() uploads
// the finished array on the main thread and swaps it in, so the old
// textures stay until the new ones are ready and a pack change never
// stalls a frame. Layer numbers follow the keys, so changing packs never
// needs a remesh.
//
// Hot reload (Section 15.3): with Graphics.Textures.HotReload on, a change
// to any file in the packs folder (saving a texture in an image editor)
// loads the packs again, live. Graphics.Textures.Folder points the game at
// another packs folder, for example the textures/ of the source tree while
// editing the default pack.
//
// Settings: Graphics.Textures.Packs, MaxResolution, Anisotropy, HotReload
// and Folder, all live (a change starts a new load).
// Stats: Render.Textures.Layers, .Resolution, .Bytes, .Warnings (gauges) and
// .LoadMs (the latest load, on a worker).
//
// Thread safety: main thread only; the load job works on a copy of what it
// needs. The destructor waits for a running load.
class TexturePackManager {
public:
    [[nodiscard]] static Result<std::unique_ptr<TexturePackManager>> create(TexturePackManagerDesc desc);
    ~TexturePackManager();
    TexturePackManager(const TexturePackManager&) = delete;
    TexturePackManager& operator=(const TexturePackManager&) = delete;
    TexturePackManager(TexturePackManager&&) = delete;
    TexturePackManager& operator=(TexturePackManager&&) = delete;

    // The block texture keys in layer order (the game's catalog), and a load
    // of them.
    void setKeys(std::vector<std::string> keys);
    // Images outside the block array to load with the textures, by path
    // inside the packs (for example "ui/font.png"). Set them before setKeys,
    // which starts the first load. A path starting with '?' is optional: no
    // warning when no pack has it ("?ui/panorama_2.jpg"); look it up without the '?'.
    void setImages(std::vector<std::string> paths);
    // A loaded image, or nothing (not requested, not loaded yet, or no pack
    // has a usable one). Changes when update() returns true.
    [[nodiscard]] const PackImage* image(std::string_view path) const noexcept;
    // Other files to load with the textures, by path inside the packs (for
    // example "ui/theme.toml"): the first pack that has one provides it.
    void setFiles(std::vector<std::string> paths);
    // A loaded file's bytes, or nothing. Changes when update() returns true.
    [[nodiscard]] const std::vector<std::byte>* file(std::string_view path) const noexcept;
    // Looks for pack folders again and loads.
    void reload();
    // Swaps in a finished load. Returns true in the frame new textures
    // became current: then textures() changed and users re-bind them.
    bool update();
    // True while a load runs or waits to be swapped in.
    [[nodiscard]] bool isLoading() const noexcept { return m_load != nullptr; }

    // The current textures, or nothing before the first load finished.
    [[nodiscard]] const BlockTextures* textures() const noexcept {
        return m_textures.texture.isValid() ? &m_textures : nullptr;
    }
    // Per key layer, for ChunkRenderer::setTextureAnimations.
    [[nodiscard]] std::span<const u32> animations() const noexcept { return m_animations; }
    // Problems of the latest load (missing textures, broken images, unknown packs).
    [[nodiscard]] std::span<const std::string> warnings() const noexcept { return m_warnings; }
    // How many times files changed and the packs were loaded again.
    [[nodiscard]] u32 hotReloads() const noexcept { return m_hotReloads; }
    // Every pack that can be enabled (folders under textures/ and the built in pack).
    [[nodiscard]] std::vector<PackManifest> availablePacks() const;
    // The packs of the latest load, highest priority first.
    [[nodiscard]] std::span<const std::string> activeStack() const noexcept { return m_activeStack; }
    // The folder the shipped packs are found in (Graphics.Textures.Folder, or textures/).
    [[nodiscard]] std::filesystem::path folder() const;
    // Where the player puts packs: what "Open Packs Folder" shows. The
    // shipped folder when there is no separate one.
    [[nodiscard]] std::filesystem::path userFolder() const {
        return m_userPacksFolder.empty() ? folder() : m_userPacksFolder;
    }

private:
    struct Load;
    explicit TexturePackManager(TexturePackManagerDesc desc);
    void registerStats();
    void scanPacks();
    void watchFolder();
    void startLoad();
    [[nodiscard]] Result<void> upload(const BlockTextureData& data);
    void destroyTextures();

    rhi::Device& m_device;
    jobs::JobSystem& m_jobs;
    stats::StatRegistry* m_stats;
    std::filesystem::path m_packsFolder;
    std::filesystem::path m_userPacksFolder;
    io::FileWatcher* m_watcher;
    std::vector<io::WatchId> m_watches;
    bool m_reloadRequested = false;
    std::unique_ptr<PackSource> m_builtIn;
    // Shared with load jobs, so a rescan never pulls packs from under one.
    std::shared_ptr<const std::vector<std::unique_ptr<PackSource>>> m_packs;
    std::vector<std::string> m_scanProblems;

    config::Setting<std::string> m_enabledPacks;
    config::Setting<i32> m_maxResolution;
    config::Setting<i32> m_anisotropy;
    config::Setting<bool> m_hotReload;
    config::Setting<std::string> m_folderSetting;
    std::vector<config::Subscription> m_subscriptions;

    std::vector<std::string> m_keys;
    std::vector<std::string> m_imagePaths;
    std::vector<PackImage> m_images;
    std::vector<std::string> m_filePaths;
    std::vector<std::pair<std::string, std::vector<std::byte>>> m_files;
    std::unique_ptr<Load> m_load;
    bool m_loadAgain = false;
    u32 m_hotReloads = 0;
    jobs::JobCounter m_jobCounter;

    BlockTextures m_textures{};
    std::vector<u32> m_animations;
    std::vector<std::string> m_warnings;
    std::vector<std::string> m_activeStack;

    struct StatIds {
        stats::StatId layers;
        stats::StatId resolution;
        stats::StatId bytes;
        stats::StatId warnings;
        stats::StatId loadMs;
    } m_statIds;
};

} // namespace vxe::render
