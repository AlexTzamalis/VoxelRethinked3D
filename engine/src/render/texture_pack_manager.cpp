#include "vxe/render/texture_pack_manager.hpp"

#include <vxe/core/assert.hpp>
#include <vxe/core/log.hpp>
#include <vxe/core/profile.hpp>

#include <algorithm>
#include <format>

namespace vxe::render {

// One load in flight: what the job reads and what it makes. The job only
// touches this object; the main thread takes it back once `done` is set.
struct TexturePackManager::Load {
    std::shared_ptr<const std::vector<std::unique_ptr<PackSource>>> packs; // Keeps the sources alive.
    std::vector<const PackSource*> stack;
    std::vector<std::string> stackIds;
    std::vector<std::string> keys;
    std::vector<std::string> imagePaths;
    std::vector<std::string> filePaths;
    BlockTextureOptions options;
    std::vector<std::string> problems; // Found while building the stack.
    BlockTextureData result;
    std::vector<PackImage> images;
    std::vector<std::pair<std::string, std::vector<std::byte>>> files;
    f64 milliseconds = 0.0;
    std::atomic<bool> done{false};
};

TexturePackManager::TexturePackManager(TexturePackManagerDesc desc)
    : m_device(*desc.device)
    , m_jobs(*desc.jobs)
    , m_stats(desc.stats)
    , m_packsFolder(std::move(desc.packsFolder))
    , m_watcher(desc.watcher)
    , m_builtIn(std::move(desc.builtIn)) {
    config::SettingsRegistry& settings = *desc.settings;
    m_enabledPacks = settings.registerString(
        {{.id = "Graphics.Textures.Packs",
          .displayName = "Texture packs",
          .description = "Enabled texture packs (folders in textures/), highest priority first, separated by "
                         "commas. The default pack is always last."},
         ""});
    m_maxResolution =
        settings.registerInt({{.id = "Graphics.Textures.MaxResolution",
                               .displayName = "Texture resolution limit",
                               .description = "Packs with larger textures are scaled down to this."},
                              256,
                              16,
                              256,
                              16});
    m_anisotropy =
        settings.registerInt({{.id = "Graphics.Textures.Anisotropy",
                               .displayName = "Anisotropic filtering",
                               .description = "Sharper textures on surfaces seen at a slant. 1 is off.",
                               .visibility = config::Visibility::Advanced},
                              8,
                              1,
                              16,
                              1});
#ifdef NDEBUG
    constexpr bool kHotReloadByDefault = false;
#else
    constexpr bool kHotReloadByDefault = true;
#endif
    m_hotReload =
        settings.registerBool({{.id = "Graphics.Textures.HotReload",
                                .displayName = "Reload changed textures",
                                .description = "Loads texture packs again when a file in them changes, "
                                               "for editing textures while the game runs."},
                               kHotReloadByDefault});
    m_folderSetting = settings.registerString(
        {{.id = "Graphics.Textures.Folder",
          .displayName = "Texture packs folder",
          .description = "Where texture packs are. Empty: textures/ next to the program.",
          .visibility = config::Visibility::Advanced},
         ""});
    const auto restart = [this](const auto& /*oldValue*/, const auto& /*newValue*/) { startLoad(); };
    m_subscriptions.push_back(m_enabledPacks.onChanged(restart));
    m_subscriptions.push_back(m_maxResolution.onChanged(restart));
    m_subscriptions.push_back(m_anisotropy.onChanged(restart));
    m_subscriptions.push_back(m_hotReload.onChanged([this](bool, bool) { watchFolder(); }));
    m_subscriptions.push_back(m_folderSetting.onChanged([this](const std::string&, const std::string&) {
        watchFolder();
        reload();
    }));
}

Result<std::unique_ptr<TexturePackManager>> TexturePackManager::create(TexturePackManagerDesc desc) {
    VXE_ASSERT(desc.device != nullptr && desc.settings != nullptr && desc.jobs != nullptr,
               "TexturePackManager needs a device, settings and jobs");
    std::unique_ptr<TexturePackManager> manager(new TexturePackManager(std::move(desc)));
    manager->registerStats();
    manager->scanPacks();
    manager->watchFolder();
    return manager;
}

TexturePackManager::~TexturePackManager() {
    if (m_watching) {
        m_watcher->unwatch(m_watch);
    }
    m_jobs.wait(m_jobCounter);
    destroyTextures();
}

void TexturePackManager::registerStats() {
    if (m_stats == nullptr) {
        return;
    }
    const auto gauge = [this](std::string_view name, std::string_view description,
                              stats::StatUnit unit = stats::StatUnit::Count) {
        auto id = m_stats->registerStat(
            {.name = name, .description = description, .type = stats::StatType::Gauge, .unit = unit});
        return id ? id.value() : stats::StatId{};
    };
    m_statIds = {
        .layers = gauge("Render.Textures.Layers", "Layers of the block texture array."),
        .resolution = gauge("Render.Textures.Resolution", "Pixels along a block texture side."),
        .bytes = gauge("Render.Textures.Bytes", "GPU memory of the block textures.", stats::StatUnit::Bytes),
        .warnings = gauge("Render.Textures.Warnings", "Problems of the latest texture pack load."),
        .loadMs = gauge("Render.Textures.LoadMs", "Time of the latest texture pack load, on a worker.",
                        stats::StatUnit::Milliseconds)};
}

std::filesystem::path TexturePackManager::folder() const {
    const std::string chosen = m_folderSetting.get();
    return chosen.empty() ? m_packsFolder : std::filesystem::path(chosen);
}

void TexturePackManager::scanPacks() {
    m_scanProblems.clear();
    auto packs = std::make_shared<std::vector<std::unique_ptr<PackSource>>>();
    std::error_code error;
    const std::filesystem::path root = folder();
    if (!root.empty() && std::filesystem::is_directory(root, error)) {
        *packs = discoverPacks(root, m_scanProblems);
    } else if (!m_folderSetting.get().empty()) {
        m_scanProblems.push_back(std::format("Texture packs folder {} does not exist", root.string()));
    }
    m_packs = std::move(packs);
}

// Watches the packs folder while hot reload is on. Any change asks for a
// reload, which update() starts once no load is running (the watcher already
// turns a burst of writes from one save into one event).
void TexturePackManager::watchFolder() {
    if (m_watcher == nullptr) {
        return;
    }
    if (m_watching) {
        m_watcher->unwatch(m_watch);
        m_watching = false;
    }
    std::error_code error;
    const std::filesystem::path root = folder();
    if (!m_hotReload.get() || root.empty() || !std::filesystem::is_directory(root, error)) {
        return;
    }
    auto watch =
        m_watcher->watch(root, true, [this](const io::FileEvent& /*event*/) { m_reloadRequested = true; });
    if (!watch) {
        VXE_LOG_WARN(log::core, "Texture packs: cannot watch {}: {}", root.string(), watch.error().message());
        return;
    }
    m_watch = watch.value();
    m_watching = true;
}

void TexturePackManager::setKeys(std::vector<std::string> keys) {
    m_keys = std::move(keys);
    startLoad();
}

void TexturePackManager::setImages(std::vector<std::string> paths) {
    m_imagePaths = std::move(paths);
    startLoad();
}

const PackImage* TexturePackManager::image(std::string_view path) const noexcept {
    const auto found = std::ranges::find(m_images, path, &PackImage::path);
    return found == m_images.end() ? nullptr : &*found;
}

void TexturePackManager::setFiles(std::vector<std::string> paths) {
    m_filePaths = std::move(paths);
    startLoad();
}

const std::vector<std::byte>* TexturePackManager::file(std::string_view path) const noexcept {
    const auto found =
        std::ranges::find(m_files, path, [](const auto& entry) { return std::string_view(entry.first); });
    return found == m_files.end() ? nullptr : &found->second;
}

void TexturePackManager::reload() {
    scanPacks();
    startLoad();
}

// Starts a load of the current keys and settings. If one is running, the
// new one starts when it finishes (its result would be outdated anyway).
void TexturePackManager::startLoad() {
    if (m_keys.empty()) {
        return;
    }
    if (m_load != nullptr) {
        m_loadAgain = true;
        return;
    }
    auto load = std::make_unique<Load>();
    load->packs = m_packs;
    load->keys = m_keys;
    load->imagePaths = m_imagePaths;
    load->filePaths = m_filePaths;
    load->options.maxResolution = static_cast<u32>(m_maxResolution.get());
    load->problems = m_scanProblems;
    // The enabled packs in order, then "default", then the built in pack.
    const std::string list = m_enabledPacks.get();
    std::vector<std::string> enabled;
    for (usize start = 0; start <= list.size();) {
        const usize comma = std::min(list.find(',', start), list.size());
        std::string id = list.substr(start, comma - start);
        std::erase(id, ' ');
        if (!id.empty() && id != "default") {
            enabled.push_back(std::move(id));
        }
        start = comma + 1;
    }
    enabled.emplace_back("default");
    for (const std::string& id : enabled) {
        const auto found =
            std::ranges::find(*m_packs, id, [](const auto& pack) { return pack->manifest().id; });
        if (found == m_packs->end()) {
            if (id != "default") {
                load->problems.push_back(
                    std::format("Texture pack '{}' is not in {}", id, folder().string()));
            }
            continue;
        }
        load->stack.push_back(found->get());
        load->stackIds.push_back(id);
    }
    if (m_builtIn != nullptr) {
        load->stack.push_back(m_builtIn.get());
        load->stackIds.push_back(m_builtIn->manifest().id + " (built in)");
    }
    Load* job = load.get();
    m_load = std::move(load);
    m_jobs.schedule(
        m_jobCounter,
        [job] {
            const Stopwatch stopwatch;
            job->result = buildBlockTextures(job->stack, job->keys, job->options);
            for (const std::string& requested : job->imagePaths) {
                const bool optional = requested.starts_with('?');
                const std::string path = optional ? requested.substr(1) : requested;
                if (auto image = readPackImage(job->stack, path, job->problems)) {
                    job->images.push_back(std::move(*image));
                } else if (!optional) {
                    job->problems.push_back(std::format("No pack has a usable {}", path));
                }
            }
            for (const std::string& path : job->filePaths) {
                const auto found = std::ranges::find_if(job->stack, [&](const PackSource* pack) {
                    if (auto bytes = pack->read(path)) {
                        job->files.emplace_back(path, std::move(*bytes));
                        return true;
                    }
                    return false;
                });
                if (found == job->stack.end()) {
                    job->problems.push_back(std::format("No pack has {}", path));
                }
            }
            job->milliseconds = stopwatch.elapsedMilliseconds();
            job->done.store(true, std::memory_order_release);
        },
        jobs::Priority::High);
}

bool TexturePackManager::update() {
    if (m_reloadRequested && m_load == nullptr) {
        m_reloadRequested = false;
        ++m_hotReloads;
        VXE_LOG_INFO(log::core, "Texture packs: files changed, loading again");
        reload();
    }
    if (m_load == nullptr || !m_load->done.load(std::memory_order_acquire)) {
        return false;
    }
    VXE_PROFILE_FUNCTION();
    const std::unique_ptr<Load> load = std::move(m_load);
    bool swapped = false;
    if (Result<void> uploaded = upload(load->result); uploaded) {
        swapped = true;
        m_animations = std::move(load->result.animations);
        m_images = std::move(load->images);
        m_files = std::move(load->files);
        m_warnings = std::move(load->problems);
        m_warnings.insert(m_warnings.end(), load->result.warnings.begin(), load->result.warnings.end());
        m_activeStack = std::move(load->stackIds);
        std::string stack;
        for (const std::string& id : m_activeStack) {
            stack += (stack.empty() ? "" : ", ") + id;
        }
        VXE_LOG_INFO(log::core, "Texture packs: {} layers at {} pixels from {} in {:.1f} ms, {} warnings",
                     load->result.layers, load->result.resolution, stack, load->milliseconds,
                     m_warnings.size());
        for (const std::string& warning : m_warnings) {
            VXE_LOG_WARN(log::core, "Texture packs: {}", warning);
        }
        if (m_stats != nullptr) {
            const auto set = [this](stats::StatId id, f64 value) {
                if (id.isValid()) {
                    m_stats->set(id, value);
                }
            };
            set(m_statIds.layers, load->result.layers);
            set(m_statIds.resolution, load->result.resolution);
            f64 bytes = 0.0;
            for (const std::vector<u32>& level : load->result.pixels) {
                bytes += static_cast<f64>(level.size() * sizeof(u32));
            }
            set(m_statIds.bytes, bytes);
            set(m_statIds.warnings, static_cast<f64>(m_warnings.size()));
            set(m_statIds.loadMs, load->milliseconds);
        }
    } else {
        VXE_LOG_ERROR(log::core, "Texture packs: upload failed: {}", uploaded.error().describe());
    }
    if (std::exchange(m_loadAgain, false)) {
        startLoad();
    }
    return swapped;
}

Result<void> TexturePackManager::upload(const BlockTextureData& data) {
    auto texture =
        m_device.createTexture({.type = rhi::TextureType::Texture2DArray,
                                .format = rhi::Format::RGBA8Srgb,
                                .width = data.resolution,
                                .height = data.resolution,
                                .layers = data.layers,
                                .mipLevels = data.mips,
                                .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst,
                                .debugName = "Block textures"});
    if (!texture) {
        return texture.error();
    }
    for (u32 mip = 0; mip < data.mips; ++mip) {
        const usize side = data.resolution >> mip;
        const usize layerTexels = side * side;
        for (u32 layer = 0; layer < data.layers; ++layer) {
            const auto pixels = std::span(data.pixels[mip]).subspan(layer * layerTexels, layerTexels);
            if (Result<void> uploaded = m_device.uploadTexture(
                    texture.value(), {.mipLevel = mip, .layer = layer, .data = std::as_bytes(pixels)});
                !uploaded) {
                m_device.destroy(texture.value());
                return uploaded.error();
            }
        }
    }
    auto sampler = m_device.createSampler({.minFilter = rhi::Filter::Linear,
                                           .magFilter = rhi::Filter::Nearest,
                                           .mipmapMode = rhi::MipmapMode::Linear,
                                           .maxAnisotropy = static_cast<f32>(m_anisotropy.get()),
                                           .debugName = "Block textures"});
    if (!sampler) {
        m_device.destroy(texture.value());
        return sampler.error();
    }
    // The device keeps destroyed objects until the GPU is done with frames
    // that used them (E6.6), so the old textures can go right away.
    destroyTextures();
    m_textures = BlockTextures{
        .texture = texture.value(), .sampler = sampler.value(), .averageColors = data.averageColors};
    return {};
}

void TexturePackManager::destroyTextures() {
    if (m_textures.texture.isValid()) {
        m_device.destroy(m_textures.texture);
    }
    if (m_textures.sampler.isValid()) {
        m_device.destroy(m_textures.sampler);
    }
    m_textures = {};
}

std::vector<PackManifest> TexturePackManager::availablePacks() const {
    std::vector<PackManifest> packs;
    for (const auto& pack : *m_packs) {
        packs.push_back(pack->manifest());
    }
    if (m_builtIn != nullptr) {
        packs.push_back(m_builtIn->manifest());
    }
    return packs;
}

} // namespace vxe::render
