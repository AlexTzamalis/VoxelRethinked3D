#pragma once

#include <vxe/core/result.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/types.hpp>
#include <vxe/rhi/command_list.hpp>
#include <vxe/rhi/device.hpp>
#include <vxe/rhi/shader_source.hpp>
#include <vxe/ui/color.hpp>
#include <vxe/ui/pixel_font.hpp>
#include <vxe/ui/quad_batch.hpp>

#include <array>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vxe::ui {

// An image to put into the UI atlas: RGBA8, red in the lowest byte, rows
// from the top (how PNGs and PackImage store them).
struct AtlasImage {
    std::string name{};
    u32 width = 0;
    u32 height = 0;
    std::span<const u32> pixels;
};

// The packed atlas: pixels top row first and where each image went.
struct AtlasPixels {
    u32 width = 0;
    u32 height = 0;
    std::vector<u32> pixels;
    std::vector<std::pair<std::string, AtlasRegion>> regions;
};

// Packs images into one texture in shelves (rows of images sorted by
// height), with a transparent texel between neighbors, starting 256 wide
// and doubling until everything fits. UI images are small and few, so this
// simple packer wastes little.
[[nodiscard]] AtlasPixels packAtlas(std::span<const AtlasImage> images);

struct UiRendererDesc {
    rhi::Device* device = nullptr;
    rhi::ShaderFileReader shaders{};
    rhi::Format targetFormat = rhi::Format::RGBA8Unorm; // The window's color format.
    stats::StatRegistry* stats = nullptr;               // Optional.
};

// Draws the game UI (Section 19.1) through the RHI. Every frame collects
// quads in a QuadBatch (rectangles, atlas images, nine slice frames, text),
// and record() draws them all with one draw call in the renderer's Ui pass
// (render::Renderer::setUi).
//
// Every quad is 48 bytes in a storage buffer that the vertex shader reads
// six times per quad (two triangles, no index buffer). Three buffers take
// turns, each guarded by a fence, so the CPU fills one while the GPU still
// reads the previous frames'. The atlas is sampled with nearest filtering
// and whole pixel positions, so pixel art and text stay crisp.
//
// Stats: Ui.Quads and Ui.DrawCalls (the latest frame), Ui.AtlasBytes.
// Thread safety: main thread only, like the device.
class UiRenderer {
public:
    static constexpr usize kFramesInFlight = 3;

    [[nodiscard]] static Result<std::unique_ptr<UiRenderer>> create(const UiRendererDesc& desc);
    ~UiRenderer();
    UiRenderer(const UiRenderer&) = delete;
    UiRenderer& operator=(const UiRenderer&) = delete;
    UiRenderer(UiRenderer&&) = delete;
    UiRenderer& operator=(UiRenderer&&) = delete;

    // Builds a new atlas from the font sheet (as PixelFont::fromSheet takes
    // it) and the named images, and uses it from now on. The old atlas stays
    // valid until the GPU is done with it.
    [[nodiscard]] Result<void> setResources(const AtlasImage& fontSheet, std::span<const AtlasImage> images);
    // The same with the font only.
    [[nodiscard]] Result<void> setFont(std::span<const u32> pixels, u32 width, u32 height);
    [[nodiscard]] const UiResources& resources() const noexcept { return m_resources; }
    [[nodiscard]] const PixelFont* font() const noexcept {
        return m_resources.font ? &*m_resources.font : nullptr;
    }
    // Goes up with every new atlas, so layouts and kept batches know when
    // text sizes or images changed.
    [[nodiscard]] u64 resourceVersion() const noexcept { return m_resources.version; }

    // Starts a frame's UI for a window of this size; drops the last frame's quads.
    void begin(u32 width, u32 height);
    // This frame's quads; draw into it or append kept batches.
    [[nodiscard]] QuadBatch& frame() noexcept { return m_frame; }
    void fillRect(const Rect& rect, u32 color) { m_frame.fillRect(rect, color); }
    void drawText(std::string_view text, f32 x, f32 y, const TextStyle& style = {}) {
        m_frame.text(text, x, y, style);
    }
    // The size of text drawn with `scale`, in window pixels (without the shadow).
    [[nodiscard]] TextSize measureText(std::string_view text, u32 scale) const noexcept;
    // Records the frame's quads into the caller's open render pass, which
    // covers the window.
    void record(rhi::CommandList& commands);

    [[nodiscard]] usize quadCount() const noexcept { return m_frame.size(); }

private:
    explicit UiRenderer(rhi::Device& device)
        : m_device(device) {}

    struct FrameBuffers {
        rhi::BufferHandle quads;
        usize capacity = 0; // Bytes.
        rhi::BufferHandle uniforms;
        rhi::FenceHandle fence;
    };

    [[nodiscard]] bool prepareSet(FrameBuffers& set, usize bytes);

    rhi::Device& m_device;
    stats::StatRegistry* m_stats = nullptr;
    stats::StatId m_quadsStat;
    stats::StatId m_drawCallsStat;
    stats::StatId m_atlasBytesStat;
    rhi::PipelineHandle m_pipeline;
    rhi::SamplerHandle m_sampler;
    rhi::TextureHandle m_atlas;
    UiResources m_resources;
    QuadBatch m_frame;
    std::array<FrameBuffers, kFramesInFlight> m_frames{};
    usize m_frameIndex = 0;
    bool m_recordedOnce = false;
    u32 m_width = 1;
    u32 m_height = 1;
};

} // namespace vxe::ui
