#pragma once

#include <vxe/core/result.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/types.hpp>
#include <vxe/rhi/command_list.hpp>
#include <vxe/rhi/device.hpp>
#include <vxe/rhi/shader_source.hpp>
#include <vxe/ui/color.hpp>
#include <vxe/ui/pixel_font.hpp>

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace vxe::ui {

// A rectangle in window pixels, origin at the top left, y down.
struct Rect {
    f32 x = 0.0F;
    f32 y = 0.0F;
    f32 width = 0.0F;
    f32 height = 0.0F;
};

struct TextStyle {
    // Window pixels per font pixel. Whole numbers keep pixel art sharp: each
    // font pixel covers the same number of window pixels.
    u32 scale = 2;
    u32 color = rgba(255, 255, 255);
    // A copy one font pixel down and right, the color darkened to a quarter,
    // which keeps text readable on any background.
    bool shadow = true;
};

struct UiRendererDesc {
    rhi::Device* device = nullptr;
    rhi::ShaderFileReader shaders{};
    rhi::Format targetFormat = rhi::Format::RGBA8Unorm; // The window's color format.
    stats::StatRegistry* stats = nullptr;               // Optional.
};

// Draws the game UI (Section 19.1) through the RHI: rectangles and pixel
// font text, collected during the frame and drawn with one draw call in the
// renderer's Ui pass (render::Renderer::setUi).
//
// Every quad is 48 bytes in a storage buffer that the vertex shader reads
// six times per quad (two triangles, no index buffer). Three buffers take
// turns, each guarded by a fence, so the CPU fills one while the GPU still
// reads the previous frames'. Positions snap to whole window pixels and the
// font texture is sampled with nearest filtering, so text stays crisp.
//
// E12.3 extends this into the general batching renderer (pack images,
// nine slice panels, clipping, rebuilding only when the widget tree
// changed).
//
// Stats: Ui.Quads and Ui.DrawCalls (gauges, the latest frame).
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

    // Uses a font sheet (RGBA8, rows from the top, as PixelFont::fromSheet
    // takes it) from now on, uploaded to a new texture. The old one stays
    // valid until the GPU is done with it.
    [[nodiscard]] Result<void> setFont(std::span<const u32> pixels, u32 width, u32 height);
    [[nodiscard]] const PixelFont* font() const noexcept { return m_font ? &*m_font : nullptr; }
    // Goes up with every setFont, so layouts know when text sizes changed.
    [[nodiscard]] u64 fontVersion() const noexcept { return m_fontVersion; }

    // Starts a frame's UI for a window of this size; drops the last frame's quads.
    void begin(u32 width, u32 height);
    void fillRect(const Rect& rect, u32 color);
    // Draws text with its top left corner at (x, y), snapped to whole pixels.
    // Without a font it draws nothing.
    void drawText(std::string_view text, f32 x, f32 y, const TextStyle& style = {});
    // The size of text drawn with `scale`, in window pixels (without the shadow).
    [[nodiscard]] TextSize measureText(std::string_view text, u32 scale) const noexcept;
    // Records the frame's quads into the caller's open render pass, which
    // covers the window.
    void record(rhi::CommandList& commands);

    [[nodiscard]] usize quadCount() const noexcept { return m_quads.size(); }

    // Must match the Quad struct in ui/quad.vert.
    struct Quad {
        std::array<f32, 4> rect{}; // x, y, width, height in window pixels.
        std::array<f32, 4> uv{};   // u0, v0, u1, v1.
        u32 color = 0;
        u32 flags = 0; // Bit 0: sample the font texture.
        std::array<u32, 2> padding{};
    };
    static_assert(sizeof(Quad) == 48);

private:
    explicit UiRenderer(rhi::Device& device)
        : m_device(device) {}

    struct FrameBuffers {
        rhi::BufferHandle quads;
        usize capacity = 0; // Bytes.
        rhi::BufferHandle uniforms;
        rhi::FenceHandle fence;
    };

    void addGlyphs(const PixelFont& font, std::string_view text, f32 x, f32 y, u32 scale, u32 color);
    [[nodiscard]] bool prepareSet(FrameBuffers& set, usize bytes);

    rhi::Device& m_device;
    stats::StatRegistry* m_stats = nullptr;
    stats::StatId m_quadsStat;
    stats::StatId m_drawCallsStat;
    rhi::PipelineHandle m_pipeline;
    rhi::SamplerHandle m_sampler;
    rhi::TextureHandle m_fontTexture;
    std::optional<PixelFont> m_font;
    u64 m_fontVersion = 0;
    std::array<FrameBuffers, kFramesInFlight> m_frames{};
    usize m_frame = 0;
    bool m_recordedOnce = false;
    u32 m_width = 1;
    u32 m_height = 1;
    std::vector<Quad> m_quads;
    std::vector<GlyphQuad> m_glyphs; // Reused layout scratch.
};

} // namespace vxe::ui
