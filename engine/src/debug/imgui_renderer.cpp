#include "vxe/debug/imgui_renderer.hpp"

#include <vxe/core/log.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <thread>
#include <vector>

namespace vxe::debug {
namespace {

static_assert(sizeof(ImDrawIdx) == 2, "The ImGui renderer binds 16 bit indices");
static_assert(sizeof(ImDrawVert) == 20, "imgui.vert reads 5 words per vertex");

// Must match the ImGuiProjection block in imgui.vert.
struct Projection {
    f32 scale[2];
    f32 translate[2];
};

// ImGuiRenderer keeps ImTextureData status bookkeeping by the rules of
// ImGui's texture protocol; these wrap the calls so the intent reads clearly.
void markReady(ImTextureData& texture, rhi::TextureHandle handle) {
    texture.SetTexID(ImGuiRenderer::textureId(handle));
    texture.SetStatus(ImTextureStatus_OK);
}

} // namespace

ImTextureID ImGuiRenderer::textureId(rhi::TextureHandle texture) noexcept {
    // 0 is ImTextureID_Invalid, so a valid handle is stored plus one.
    return texture.isValid() ? static_cast<ImTextureID>(texture.packed()) + 1 : ImTextureID_Invalid;
}

rhi::TextureHandle ImGuiRenderer::textureHandle(ImTextureID id) noexcept {
    if (id == ImTextureID_Invalid) {
        return {};
    }
    const auto packed = static_cast<u32>(id - 1);
    return rhi::TextureHandle::make(packed & rhi::TextureHandle::kInvalidIndex,
                                    static_cast<u8>(packed >> 24U));
}

Result<std::unique_ptr<ImGuiRenderer>>
ImGuiRenderer::create(rhi::Device& device, const rhi::ShaderFileReader& shaders, rhi::Format targetFormat) {
    std::unique_ptr<ImGuiRenderer> renderer(new ImGuiRenderer(device));
    const auto vertex = rhi::createShaderFromFile(device, "debug/imgui.vert", shaders);
    if (!vertex) {
        return vertex.error().withContext("ImGui renderer");
    }
    const auto fragment = rhi::createShaderFromFile(device, "debug/imgui.frag", shaders);
    if (!fragment) {
        device.destroy(vertex.value());
        return fragment.error().withContext("ImGui renderer");
    }
    rhi::PipelineDesc desc{.vertex = vertex.value(), .fragment = fragment.value(), .debugName = "ImGui"};
    desc.raster.cullMode = rhi::CullMode::None;
    desc.depth.testEnabled = false;
    desc.depth.writeEnabled = false;
    desc.blend[0] = rhi::BlendState::alphaBlend();
    desc.colorFormats[0] = targetFormat;
    desc.colorCount = 1;
    auto pipeline = device.createPipeline(desc);
    device.destroy(vertex.value());
    device.destroy(fragment.value());
    if (!pipeline) {
        return pipeline.error().withContext("ImGui renderer");
    }
    renderer->m_pipeline = pipeline.value();
    auto sampler = device.createSampler({.minFilter = rhi::Filter::Linear,
                                         .magFilter = rhi::Filter::Linear,
                                         .mipmapMode = rhi::MipmapMode::None,
                                         .addressU = rhi::AddressMode::ClampToEdge,
                                         .addressV = rhi::AddressMode::ClampToEdge,
                                         .debugName = "ImGui sampler"});
    if (!sampler) {
        return sampler.error().withContext("ImGui renderer");
    }
    renderer->m_sampler = sampler.value();
    for (FrameBuffers& set : renderer->m_frames) {
        auto uniforms = device.createBuffer({.sizeBytes = 256,
                                             .usage = rhi::BufferUsage::Uniform | rhi::BufferUsage::CpuWrite,
                                             .debugName = "ImGui projection"});
        if (!uniforms) {
            return uniforms.error().withContext("ImGui renderer");
        }
        set.uniforms = uniforms.value();
    }
    return renderer;
}

ImGuiRenderer::~ImGuiRenderer() {
    for (const FrameBuffers& set : m_frames) {
        m_device.destroy(set.vertices);
        m_device.destroy(set.indices);
        m_device.destroy(set.uniforms);
        m_device.destroy(set.fence);
    }
    m_device.destroy(m_pipeline);
    m_device.destroy(m_sampler);
}

void ImGuiRenderer::createTexture(ImTextureData& texture) {
    const auto created = m_device.createTexture(
        {.format = rhi::Format::RGBA8Unorm,
         .width = static_cast<u32>(texture.Width),
         .height = static_cast<u32>(texture.Height),
         .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst,
         .debugName = std::format("ImGui texture {}x{}", texture.Width, texture.Height)});
    if (!created) {
        VXE_LOG_ERROR(log::rhi, "ImGui texture: {}", created.error().describe());
        texture.SetStatus(ImTextureStatus_Destroyed);
        return;
    }
    markReady(texture, created.value());
    uploadTexture(texture);
}

// Uploads the whole texture. ImGui also offers the changed rectangles, but
// the RHI uploads whole mip levels, and atlas updates are rare and small
// enough (a new glyph now and then) that a full upload costs nothing visible.
void ImGuiRenderer::uploadTexture(ImTextureData& texture) {
    const rhi::TextureHandle handle = textureHandle(texture.GetTexID());
    const auto pixelCount = static_cast<usize>(texture.Width) * static_cast<usize>(texture.Height);
    // Rows go up in ImGui's order (top first) and are sampled the same way,
    // because ImGui's uv v = 0 also means "first row of the data".
    std::span<const std::byte> pixels(static_cast<const std::byte*>(texture.GetPixels()),
                                      pixelCount * static_cast<usize>(texture.BytesPerPixel));
    std::vector<std::byte> expanded;
    if (texture.Format == ImTextureFormat_Alpha8) {
        // The RHI texture is RGBA8: white with the coverage as alpha.
        expanded.resize(pixelCount * 4);
        for (usize i = 0; i < pixelCount; ++i) {
            expanded[i * 4 + 0] = std::byte{0xFF};
            expanded[i * 4 + 1] = std::byte{0xFF};
            expanded[i * 4 + 2] = std::byte{0xFF};
            expanded[i * 4 + 3] = pixels[i];
        }
        pixels = expanded;
    }
    if (auto result = m_device.uploadTexture(handle, {.mipLevel = 0, .layer = 0, .data = pixels}); !result) {
        VXE_LOG_ERROR(log::rhi, "ImGui texture upload: {}", result.error().describe());
    }
    texture.SetStatus(ImTextureStatus_OK);
}

void ImGuiRenderer::destroyTexture(ImTextureData& texture) {
    m_device.destroy(textureHandle(texture.GetTexID()));
    texture.SetTexID(ImTextureID_Invalid);
    texture.SetStatus(ImTextureStatus_Destroyed);
}

void ImGuiRenderer::updateTextures(ImDrawData& drawData) {
    if (drawData.Textures == nullptr) {
        return;
    }
    for (ImTextureData* texture : *drawData.Textures) {
        switch (texture->Status) {
        case ImTextureStatus_WantCreate:
            createTexture(*texture);
            break;
        case ImTextureStatus_WantUpdates:
            uploadTexture(*texture);
            break;
        case ImTextureStatus_WantDestroy:
            // ImGui waits a few frames (UnusedFrames) before asking, but the
            // RHI also defers the real deletion until the GPU is done (E6.6).
            destroyTexture(*texture);
            break;
        case ImTextureStatus_OK:
        case ImTextureStatus_Destroyed:
            break;
        }
    }
}

void ImGuiRenderer::releaseTextures() {
    for (ImTextureData* texture : ImGui::GetPlatformIO().Textures) {
        if (texture->Status != ImTextureStatus_Destroyed && texture->GetTexID() != ImTextureID_Invalid) {
            destroyTexture(*texture);
        }
    }
}

Result<void> ImGuiRenderer::ensureCapacity(rhi::BufferHandle& buffer, usize& capacity, usize needed,
                                           rhi::BufferUsage usage, std::string_view name) {
    if (capacity >= needed && buffer.isValid()) {
        return {};
    }
    // Grow to a power of two, so a slowly growing UI does not reallocate
    // every frame. The old buffer's deletion is deferred until the GPU is done.
    const usize size = std::max<usize>(std::bit_ceil(needed), usize{64} * 1024);
    auto created = m_device.createBuffer(
        {.sizeBytes = size, .usage = usage | rhi::BufferUsage::CpuWrite, .debugName = name});
    if (!created) {
        return created.error();
    }
    m_device.destroy(buffer);
    buffer = created.value();
    capacity = size;
    return {};
}

// Makes sure the GPU no longer reads `set` before the CPU overwrites it. With
// three sets the GPU would have to be three frames behind for this to wait.
void ImGuiRenderer::waitForSet(FrameBuffers& set) {
    if (!set.fence.isValid()) {
        return;
    }
    while (!m_device.isFenceSignaled(set.fence)) {
        std::this_thread::yield();
    }
    m_device.destroy(set.fence);
    set.fence = {};
}

void ImGuiRenderer::bindState(rhi::CommandList& commands, const FrameBuffers& set, usize vertexBytes) const {
    commands.bindPipeline(m_pipeline);
    commands.bindUniformBuffer(1, 0, set.uniforms, 0, sizeof(Projection));
    commands.bindStorageBuffer(2, 0, set.vertices, 0, vertexBytes);
    commands.bindIndexBuffer(set.indices, rhi::IndexType::Uint16);
}

void ImGuiRenderer::record(rhi::CommandList& commands, const ImDrawData& drawData) {
    const f32 framebufferWidth = drawData.DisplaySize.x * drawData.FramebufferScale.x;
    const f32 framebufferHeight = drawData.DisplaySize.y * drawData.FramebufferScale.y;
    if (drawData.TotalVtxCount == 0 || framebufferWidth <= 0.0F || framebufferHeight <= 0.0F) {
        return;
    }

    // The previous frame's set gets a fence now: it is inserted after that
    // frame was submitted, so it signals once the GPU has finished it.
    if (m_recordedOnce) {
        FrameBuffers& previous = m_frames[m_frame];
        m_device.destroy(previous.fence);
        previous.fence = m_device.insertFence();
        m_frame = (m_frame + 1) % kFramesInFlight;
    }
    m_recordedOnce = true;
    FrameBuffers& set = m_frames[m_frame];
    waitForSet(set);

    const usize vertexBytes = static_cast<usize>(drawData.TotalVtxCount) * sizeof(ImDrawVert);
    const usize indexBytes = static_cast<usize>(drawData.TotalIdxCount) * sizeof(ImDrawIdx);
    if (!ensureCapacity(set.vertices, set.vertexCapacity, vertexBytes, rhi::BufferUsage::Storage,
                        "ImGui vertices") ||
        !ensureCapacity(set.indices, set.indexCapacity, indexBytes, rhi::BufferUsage::Index,
                        "ImGui indices")) {
        VXE_LOG_ERROR(log::rhi, "ImGui renderer: cannot grow its buffers; this frame is not drawn");
        return;
    }

    // Copy every list's vertices and indices into one buffer each; draws then
    // address them with a running vertex and index offset.
    std::byte* vertexOut = m_device.mappedRange(set.vertices).data();
    std::byte* indexOut = m_device.mappedRange(set.indices).data();
    for (const ImDrawList* list : drawData.CmdLists) {
        const auto listVertexBytes = static_cast<usize>(list->VtxBuffer.Size) * sizeof(ImDrawVert);
        const auto listIndexBytes = static_cast<usize>(list->IdxBuffer.Size) * sizeof(ImDrawIdx);
        std::memcpy(vertexOut, list->VtxBuffer.Data, listVertexBytes);
        std::memcpy(indexOut, list->IdxBuffer.Data, listIndexBytes);
        vertexOut += listVertexBytes;
        indexOut += listIndexBytes;
    }

    // ImGui's coordinates: origin at DisplayPos, y down. Clip space: y up.
    const Projection projection{.scale = {2.0F / drawData.DisplaySize.x, -2.0F / drawData.DisplaySize.y},
                                .translate = {-1.0F - drawData.DisplayPos.x * 2.0F / drawData.DisplaySize.x,
                                              1.0F + drawData.DisplayPos.y * 2.0F / drawData.DisplaySize.y}};
    std::memcpy(m_device.mappedRange(set.uniforms).data(), &projection, sizeof(projection));

    commands.beginDebugGroup("ImGui");
    commands.setViewport(0.0F, 0.0F, framebufferWidth, framebufferHeight);
    bindState(commands, set, vertexBytes);

    const ImVec2 clipOffset = drawData.DisplayPos;
    const ImVec2 clipScale = drawData.FramebufferScale;
    rhi::TextureHandle boundTexture;
    u32 globalVertex = 0;
    u32 globalIndex = 0;
    for (const ImDrawList* list : drawData.CmdLists) {
        for (const ImDrawCmd& command : list->CmdBuffer) {
            if (command.UserCallback != nullptr) {
                if (command.UserCallback == ImDrawCallback_ResetRenderState) {
                    bindState(commands, set, vertexBytes);
                    boundTexture = {};
                } else {
                    command.UserCallback(list, &command);
                }
                continue;
            }
            // The clip rectangle in framebuffer pixels, clamped to the target.
            const f32 minX = std::max((command.ClipRect.x - clipOffset.x) * clipScale.x, 0.0F);
            const f32 minY = std::max((command.ClipRect.y - clipOffset.y) * clipScale.y, 0.0F);
            const f32 maxX = std::min((command.ClipRect.z - clipOffset.x) * clipScale.x, framebufferWidth);
            const f32 maxY = std::min((command.ClipRect.w - clipOffset.y) * clipScale.y, framebufferHeight);
            if (maxX <= minX || maxY <= minY) {
                continue;
            }
            // Scissors use the lower left origin (ADR-016), ImGui the upper left.
            commands.setScissor(static_cast<i32>(minX), static_cast<i32>(framebufferHeight - maxY),
                                static_cast<u32>(maxX - minX), static_cast<u32>(maxY - minY));
            const rhi::TextureHandle texture = textureHandle(command.GetTexID());
            if (texture != boundTexture) {
                commands.bindTexture(2, 1, texture, m_sampler);
                boundTexture = texture;
            }
            commands.drawIndexed(command.ElemCount, 1, globalIndex + command.IdxOffset,
                                 static_cast<i32>(globalVertex + command.VtxOffset), 0);
        }
        globalVertex += static_cast<u32>(list->VtxBuffer.Size);
        globalIndex += static_cast<u32>(list->IdxBuffer.Size);
    }
    commands.endDebugGroup();
}

} // namespace vxe::debug
