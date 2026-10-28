#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>
#include <vxe/math/vector.hpp>
#include <vxe/render/cpu_write_ring.hpp>
#include <vxe/rhi/command_list.hpp>
#include <vxe/rhi/device.hpp>
#include <vxe/rhi/shader_source.hpp>

#include <memory>

namespace vxe::render {

enum class Tonemapper : u8 {
    None,     // Clamp at 1: what was rendered, with hard clipping of highlights.
    Reinhard, // x / (1 + x): soft, never clips, a little flat.
    Aces,     // Narkowicz's ACES fit: filmic contrast, the default.
};

// The tonemapping curves of shaders/render/post.frag, on the CPU, for tests
// and tools. `color` is linear HDR; the result is linear display color in [0, 1].
[[nodiscard]] Vec3 tonemap(Vec3 color, Tonemapper curve, f32 exposure) noexcept;
// Linear to sRGB encoded, per channel, like the shader.
[[nodiscard]] f32 encodeSrgb(f32 linear) noexcept;
// sRGB encoded to linear, for colors picked on screen (sky colors, UI).
[[nodiscard]] f32 decodeSrgb(f32 encoded) noexcept;
[[nodiscard]] Vec3 decodeSrgb(Vec3 encoded) noexcept;

struct PostProcessDesc {
    rhi::Device* device = nullptr;
    config::SettingsRegistry* settings = nullptr;
    rhi::ShaderFileReader shaders{};
    rhi::Format outputFormat = rhi::Format::RGBA8Unorm; // The window.
};

// Post processing (Section 8.1 pass 9, E8.4): reads the HDR scene and writes
// the window in one fullscreen pass: exposure, tonemapping, optional FXAA,
// sRGB encoding.
//
// Settings, all live: Graphics.Post.Tonemapper (None, Reinhard, Aces),
// Graphics.Post.Exposure and Graphics.Post.Fxaa.
//
// Thread safety: main thread only.
class PostProcess {
public:
    [[nodiscard]] static Result<std::unique_ptr<PostProcess>> create(const PostProcessDesc& desc);
    ~PostProcess();
    PostProcess(const PostProcess&) = delete;
    PostProcess& operator=(const PostProcess&) = delete;
    PostProcess(PostProcess&&) = delete;
    PostProcess& operator=(PostProcess&&) = delete;

    // Reads the settings and uploads this frame's parameters for an HDR scene
    // of the given size. Once per frame, before recording.
    void update(u32 sceneWidth, u32 sceneHeight);
    // Draws the scene into the current render pass (the window).
    void record(rhi::CommandList& commands, rhi::TextureHandle hdrScene) const;

private:
    explicit PostProcess(const PostProcessDesc& desc);

    rhi::Device& m_device;
    rhi::PipelineHandle m_pipeline;
    rhi::SamplerHandle m_sampler;
    CpuWriteRing m_ring;
    config::Setting<Tonemapper> m_tonemapper;
    config::Setting<f32> m_exposure;
    config::Setting<bool> m_fxaa;
};

} // namespace vxe::render
