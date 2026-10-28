#include "vxe/render/frame_graph.hpp"

#include <vxe/core/assert.hpp>
#include <vxe/core/log.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <utility>

namespace vxe::render {
namespace {

using rhi::BarrierScope;
using rhi::LoadOp;

// The barrier bits that make earlier shader storage writes visible to an
// access of this kind. Attachments have no bit of their own in our scope
// set (OpenGL calls it the framebuffer barrier), so they take all bits,
// which costs nothing measurable for the rare compute then render case.
BarrierScope scopeOf(BufferAccess access) noexcept {
    switch (access) {
    case BufferAccess::Vertex:
        return BarrierScope::VertexData;
    case BufferAccess::Index:
        return BarrierScope::IndexData;
    case BufferAccess::Uniform:
        return BarrierScope::UniformData;
    case BufferAccess::Indirect:
        return BarrierScope::IndirectCommands;
    case BufferAccess::StorageRead:
    case BufferAccess::StorageWrite:
        return BarrierScope::StorageData;
    case BufferAccess::TransferSrc:
    case BufferAccess::TransferDst:
        return BarrierScope::TransferData;
    }
    return BarrierScope::All;
}

bool isValidPassName(std::string_view name) noexcept {
    return !name.empty() &&
           std::ranges::all_of(name, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; });
}

} // namespace

rhi::TextureHandle PassContext::texture(TextureRef ref) const {
    return graph.texture(ref);
}

rhi::BufferHandle PassContext::buffer(BufferRef ref) const {
    return graph.buffer(ref);
}

// ---------------------------------------------------------------------------
// PassBuilder

TextureRef PassBuilder::createTexture(std::string_view name, const TransientTextureDesc& desc) {
    VXE_ASSERT(!m_graph->m_compiled, "Frame graph resources must be declared before compile");
    m_graph->m_textures.push_back({.name = std::string(name), .desc = desc});
    return TextureRef{static_cast<u32>(m_graph->m_textures.size() - 1)};
}

void PassBuilder::colorAttachment(TextureRef texture, rhi::LoadOp load, std::array<f32, 4> clearColor,
                                  rhi::StoreOp store) {
    m_graph->m_passes[m_pass].textures.push_back({.texture = texture,
                                                  .access = FrameGraph::TextureAccess::ColorAttachment,
                                                  .load = load,
                                                  .store = store,
                                                  .clearColor = clearColor});
}

void PassBuilder::depthAttachment(TextureRef texture, rhi::LoadOp load, f32 clearDepth, rhi::StoreOp store) {
    m_graph->m_passes[m_pass].textures.push_back({.texture = texture,
                                                  .access = FrameGraph::TextureAccess::DepthAttachment,
                                                  .load = load,
                                                  .store = store,
                                                  .clearDepth = clearDepth});
}

void PassBuilder::windowAttachment(rhi::LoadOp load, std::array<f32, 4> clearColor) {
    FrameGraph::Pass& pass = m_graph->m_passes[m_pass];
    pass.window = true;
    pass.windowLoad = load;
    pass.windowClear = clearColor;
}

void PassBuilder::sampleTexture(TextureRef texture) {
    m_graph->m_passes[m_pass].textures.push_back(
        {.texture = texture, .access = FrameGraph::TextureAccess::Sampled});
}

void PassBuilder::readStorageTexture(TextureRef texture) {
    m_graph->m_passes[m_pass].textures.push_back(
        {.texture = texture, .access = FrameGraph::TextureAccess::StorageRead});
}

void PassBuilder::writeStorageTexture(TextureRef texture) {
    m_graph->m_passes[m_pass].textures.push_back(
        {.texture = texture, .access = FrameGraph::TextureAccess::StorageWrite});
}

void PassBuilder::useBuffer(BufferRef buffer, BufferAccess access) {
    m_graph->m_passes[m_pass].buffers.push_back({.buffer = buffer, .access = access});
}

// ---------------------------------------------------------------------------
// FrameGraph setup

FrameGraph::FrameGraph(const FrameGraphDesc& desc)
    : m_device(*desc.device)
    , m_settings(desc.settings)
    , m_stats(desc.stats) {}

FrameGraph::~FrameGraph() {
    releaseTextures();
    for (const Pass& pass : m_passes) {
        if (pass.timer) {
            pass.timer->destroy(m_device);
        }
    }
}

PassBuilder FrameGraph::addPass(PassDesc desc) {
    VXE_ASSERT(!m_compiled, "Passes must be added before compile");
    m_passes.push_back({.desc = std::move(desc)});
    return PassBuilder(*this, static_cast<u32>(m_passes.size() - 1));
}

TextureRef FrameGraph::importTexture(std::string_view name, rhi::TextureHandle texture) {
    VXE_ASSERT(!m_compiled, "Frame graph resources must be declared before compile");
    m_textures.push_back({.name = std::string(name), .imported = true, .handle = texture});
    return TextureRef{static_cast<u32>(m_textures.size() - 1)};
}

BufferRef FrameGraph::importBuffer(std::string_view name, rhi::BufferHandle buffer) {
    VXE_ASSERT(!m_compiled, "Frame graph resources must be declared before compile");
    m_buffers.push_back({.name = std::string(name), .handle = buffer});
    return BufferRef{static_cast<u32>(m_buffers.size() - 1)};
}

void FrameGraph::setImportedTexture(TextureRef ref, rhi::TextureHandle texture) {
    VXE_ASSERT(ref.index < m_textures.size() && m_textures[ref.index].imported,
               "Only imported textures can be replaced");
    m_textures[ref.index].handle = texture;
}

void FrameGraph::setImportedBuffer(BufferRef ref, rhi::BufferHandle buffer) {
    VXE_ASSERT(ref.index < m_buffers.size(), "Unknown buffer");
    m_buffers[ref.index].handle = buffer;
}

Error FrameGraph::error(const Pass& pass, std::string_view message) {
    return Error(ErrorCode::InvalidArgument,
                 std::format("Frame graph pass '{}': {}", pass.desc.name, message));
}

Result<void> FrameGraph::validate() const {
    std::vector<bool> written(m_textures.size(), false);
    for (usize i = 0; i < m_textures.size(); ++i) {
        written[i] = m_textures[i].imported; // Imported textures arrive with contents.
    }
    for (usize p = 0; p < m_passes.size(); ++p) {
        VXE_TRY(validateShape(p));
        VXE_TRY(validateTextureUses(m_passes[p], written));
    }
    return {};
}

// Name, record function, references and attachment counts of one pass.
Result<void> FrameGraph::validateShape(usize index) const {
    const Pass& pass = m_passes[index];
    if (!isValidPassName(pass.desc.name)) {
        return error(pass, "names may only use letters and digits");
    }
    const bool duplicate =
        std::ranges::any_of(m_passes.begin(), m_passes.begin() + static_cast<std::ptrdiff_t>(index),
                            [&pass](const Pass& other) { return other.desc.name == pass.desc.name; });
    if (duplicate) {
        return error(pass, "the name is used twice");
    }
    if (!pass.desc.record) {
        return error(pass, "has no record function");
    }
    const bool foreignTexture = std::ranges::any_of(
        pass.textures, [this](const TextureUse& use) { return use.texture.index >= m_textures.size(); });
    const bool foreignBuffer = std::ranges::any_of(
        pass.buffers, [this](const BufferUse& use) { return use.buffer.index >= m_buffers.size(); });
    if (foreignTexture || foreignBuffer) {
        return error(pass, "uses a resource of another graph");
    }
    const auto colors = std::ranges::count_if(
        pass.textures, [](const TextureUse& use) { return use.access == TextureAccess::ColorAttachment; });
    const auto depths = std::ranges::count_if(
        pass.textures, [](const TextureUse& use) { return use.access == TextureAccess::DepthAttachment; });
    if (pass.desc.type != PassType::Raster) {
        if (colors > 0 || depths > 0 || pass.window) {
            return error(pass, "only raster passes have attachments");
        }
        return {};
    }
    if (colors == 0 && depths == 0 && !pass.window) {
        return error(pass, "a raster pass needs an attachment");
    }
    if (pass.window && (colors > 0 || depths > 0)) {
        return error(pass, "the window cannot be combined with texture attachments in one pass");
    }
    if (std::cmp_greater(colors, rhi::kMaxColorAttachments) || depths > 1) {
        return error(pass, std::format("at most {} color attachments and one depth attachment",
                                       rhi::kMaxColorAttachments));
    }
    return {};
}

// Ordering and format rules of the pass's texture accesses. `written` holds
// which textures earlier passes wrote and is updated with this pass's writes.
Result<void> FrameGraph::validateTextureUses(const Pass& pass, std::vector<bool>& written) const {
    for (const TextureUse& use : pass.textures) {
        const Texture& texture = m_textures[use.texture.index];
        const bool attachment =
            use.access == TextureAccess::ColorAttachment || use.access == TextureAccess::DepthAttachment;
        if (attachment) {
            // Rendering into a texture while sampling it is a feedback loop:
            // undefined results on every API.
            const bool alsoRead = std::ranges::any_of(pass.textures, [&use](const TextureUse& other) {
                return other.texture == use.texture && (other.access == TextureAccess::Sampled ||
                                                        other.access == TextureAccess::StorageRead ||
                                                        other.access == TextureAccess::StorageWrite);
            });
            if (alsoRead) {
                return error(pass,
                             std::format("renders into '{}' and reads it at the same time", texture.name));
            }
            const bool wantsDepth = use.access == TextureAccess::DepthAttachment;
            if (!texture.imported && wantsDepth != rhi::isDepthFormat(texture.desc.format)) {
                return error(pass,
                             std::format("'{}' has the wrong format for this attachment", texture.name));
            }
            if (use.load == LoadOp::Load && !written[use.texture.index]) {
                return error(pass, std::format("loads '{}' before any pass writes it", texture.name));
            }
        }
        const bool reads = use.access == TextureAccess::Sampled || use.access == TextureAccess::StorageRead;
        if (reads && !written[use.texture.index]) {
            return error(pass, std::format("reads '{}' before any pass writes it", texture.name));
        }
    }
    for (const TextureUse& use : pass.textures) {
        if (use.access != TextureAccess::Sampled && use.access != TextureAccess::StorageRead) {
            written[use.texture.index] = true;
        }
    }
    return {};
}

void FrameGraph::planBarriers() {
    // Per resource: whether a shader storage write is waiting to be made
    // visible, and which barrier bits were already issued since that write.
    struct State {
        bool pending = false;
        BarrierScope covered = BarrierScope::None;
    };
    std::vector<State> textures(m_textures.size());
    std::vector<State> buffers(m_buffers.size());
    const auto need = [](State& state, BarrierScope scope, BarrierScope& barrier) {
        if (state.pending && (state.covered & scope) != scope) {
            barrier |= scope;
            state.covered |= scope;
        }
    };
    // Imported resources keep their contents from one frame to the next, so
    // a storage write late in a frame (the Hi-Z build) must be visible to an
    // early pass of the next frame (culling). Walking the passes twice and
    // keeping the second walk's barriers gives the steady state for that.
    // Transient textures start every frame fresh.
    for (int walk = 0; walk < 2; ++walk) {
        for (usize i = 0; i < m_textures.size(); ++i) {
            if (!m_textures[i].imported) {
                textures[i] = {};
            }
        }
        for (Pass& pass : m_passes) {
            BarrierScope barrier = BarrierScope::None;
            for (const TextureUse& use : pass.textures) {
                BarrierScope scope = BarrierScope::All;
                if (use.access == TextureAccess::Sampled) {
                    scope = BarrierScope::TextureFetch;
                } else if (use.access == TextureAccess::StorageRead ||
                           use.access == TextureAccess::StorageWrite) {
                    scope = BarrierScope::StorageImage;
                }
                need(textures[use.texture.index], scope, barrier);
            }
            for (const BufferUse& use : pass.buffers) {
                need(buffers[use.buffer.index], scopeOf(use.access), barrier);
            }
            // Writes become pending after the pass, for the passes that follow.
            for (const TextureUse& use : pass.textures) {
                if (use.access == TextureAccess::StorageWrite) {
                    textures[use.texture.index] = {.pending = true};
                }
            }
            for (const BufferUse& use : pass.buffers) {
                if (use.access == BufferAccess::StorageWrite) {
                    buffers[use.buffer.index] = {.pending = true};
                }
            }
            pass.barrier = barrier;
        }
    }
    m_barrierCount = static_cast<u32>(
        std::ranges::count_if(m_passes, [](const Pass& pass) { return pass.barrier != BarrierScope::None; }));
}

void FrameGraph::planDisabledClears() {
    for (Pass& pass : m_passes) {
        pass.clearsWhenDisabled = pass.desc.type == PassType::Raster &&
                                  ((pass.window && pass.windowLoad != LoadOp::Load) ||
                                   std::ranges::any_of(pass.textures, [](const TextureUse& use) {
                                       return (use.access == TextureAccess::ColorAttachment ||
                                               use.access == TextureAccess::DepthAttachment) &&
                                              use.load != LoadOp::Load;
                                   }));
    }
}

void FrameGraph::registerControls() {
    if (m_settings != nullptr) {
        m_aliasing = m_settings->registerBool(
            {{.id = "Debug.FrameGraph.Aliasing",
              .displayName = "Share render targets",
              .description = "Transient render targets with the same format and size share GPU memory when "
                             "their passes do not overlap.",
              .visibility = config::Visibility::Developer},
             true});
    }
    for (Pass& pass : m_passes) {
        if (m_settings != nullptr && pass.desc.canDisable) {
            const std::string id = std::format("Debug.Passes.{}", pass.desc.name);
            const std::string displayName = std::format("{} pass", pass.desc.name);
            pass.enabled = m_settings->registerBool({{.id = id,
                                                      .displayName = displayName,
                                                      .description = "Runs this render pass. Off skips it, "
                                                                     "and its targets are only cleared.",
                                                      .visibility = config::Visibility::Developer},
                                                     true});
            pass.hasToggle = true;
        }
        if (m_stats != nullptr) {
            const std::string name = std::format("Gpu.Pass.{}", pass.desc.name);
            const std::string description =
                std::format("GPU time of the {} pass, a few frames late.", pass.desc.name);
            auto stat = m_stats->registerStat({.name = name,
                                               .description = description,
                                               .type = stats::StatType::Gauge,
                                               .unit = stats::StatUnit::Milliseconds,
                                               .historyFrames = 240});
            if (stat) {
                pass.gpuTime = stat.value();
            }
        }
        if (m_device.capabilities().timestampQueries) {
            auto timer = std::make_unique<rhi::GpuFrameTimer>();
            if (timer->create(m_device)) {
                pass.timer = std::move(timer);
            }
        }
    }
    if (m_stats != nullptr) {
        const auto stat = [this](const stats::StatDesc& desc) {
            auto id = m_stats->registerStat(desc);
            return id ? id.value() : stats::StatId{};
        };
        m_transientBytesStat = stat({.name = "Render.FrameGraph.TransientMemory",
                                     .description = "GPU memory of the frame graph's transient textures.",
                                     .type = stats::StatType::Gauge,
                                     .unit = stats::StatUnit::Bytes});
        m_barriersStat = stat({.name = "Render.FrameGraph.Barriers",
                               .description = "Memory barriers the frame graph recorded this frame."});
        m_passesStat = stat({.name = "Render.FrameGraph.PassesRun",
                             .description = "Frame graph passes that ran this frame (not disabled)."});
    }
}

Result<void> FrameGraph::compile() {
    if (m_compiled) {
        return Error(ErrorCode::AlreadyExists, "The frame graph is already compiled");
    }
    VXE_TRY(validate());
    for (Texture& texture : m_textures) {
        texture.firstPass.reset();
        texture.usage = texture.desc.extraUsage;
    }
    for (u32 p = 0; p < m_passes.size(); ++p) {
        for (const TextureUse& use : m_passes[p].textures) {
            Texture& texture = m_textures[use.texture.index];
            if (!texture.firstPass) {
                texture.firstPass = p;
            }
            texture.lastPass = p;
            switch (use.access) {
            case TextureAccess::ColorAttachment:
                texture.usage |= rhi::TextureUsage::RenderTarget;
                break;
            case TextureAccess::DepthAttachment:
                texture.usage |= rhi::TextureUsage::DepthTarget;
                break;
            case TextureAccess::Sampled:
                texture.usage |= rhi::TextureUsage::Sampled;
                break;
            case TextureAccess::StorageRead:
            case TextureAccess::StorageWrite:
                texture.usage |= rhi::TextureUsage::Storage;
                break;
            }
        }
    }
    planBarriers();
    planDisabledClears();
    registerControls();
    m_compiled = true;
    return {};
}

// ---------------------------------------------------------------------------
// Transient textures

void FrameGraph::setOutputSize(u32 width, u32 height) noexcept {
    m_width = std::max(width, 1U);
    m_height = std::max(height, 1U);
}

u32 FrameGraph::textureWidth(const Texture& texture) const noexcept {
    if (texture.desc.width > 0 && texture.desc.height > 0) {
        return texture.desc.width;
    }
    return std::max(1U, static_cast<u32>(std::lround(static_cast<f32>(m_width) * texture.desc.scale)));
}

u32 FrameGraph::textureHeight(const Texture& texture) const noexcept {
    if (texture.desc.width > 0 && texture.desc.height > 0) {
        return texture.desc.height;
    }
    return std::max(1U, static_cast<u32>(std::lround(static_cast<f32>(m_height) * texture.desc.scale)));
}

void FrameGraph::releaseTextures() {
    // The device defers the destruction until the GPU finished the frames
    // that still use these textures (E6.6), so this is safe mid run.
    for (const Physical& physical : m_physical) {
        m_device.destroy(physical.handle);
    }
    m_physical.clear();
    m_transientBytes = 0;
    m_allocated = false;
}

Result<void> FrameGraph::allocate() {
    releaseTextures();
    const bool aliasing = m_settings == nullptr || m_aliasing.get();
    // Textures in order of their first pass, each placed in the first GPU
    // texture with an identical description that is free by then. Equal
    // descriptions only: OpenGL cannot view one allocation as another format.
    std::vector<u32> order;
    for (u32 i = 0; i < m_textures.size(); ++i) {
        if (!m_textures[i].imported && m_textures[i].firstPass) {
            order.push_back(i);
        }
    }
    std::ranges::stable_sort(order, {}, [this](u32 i) { return *m_textures[i].firstPass; });
    for (const u32 index : order) {
        Texture& texture = m_textures[index];
        const u32 width = textureWidth(texture);
        const u32 height = textureHeight(texture);
        const auto shared = std::ranges::find_if(m_physical, [&](const Physical& physical) {
            return aliasing && physical.format == texture.desc.format && physical.width == width &&
                   physical.height == height && physical.usage == texture.usage &&
                   physical.lastPass < *texture.firstPass;
        });
        if (shared != m_physical.end()) {
            shared->lastPass = texture.lastPass;
            shared->debugName += "+" + texture.name;
            texture.physical = static_cast<u32>(shared - m_physical.begin());
            continue;
        }
        m_physical.push_back({.format = texture.desc.format,
                              .width = width,
                              .height = height,
                              .usage = texture.usage,
                              .lastPass = texture.lastPass,
                              .debugName = "Frame graph " + texture.name});
        texture.physical = static_cast<u32>(m_physical.size() - 1);
    }
    for (Physical& physical : m_physical) {
        auto created = m_device.createTexture({.format = physical.format,
                                               .width = physical.width,
                                               .height = physical.height,
                                               .usage = physical.usage,
                                               .debugName = physical.debugName});
        if (!created) {
            releaseTextures();
            return created.error();
        }
        physical.handle = created.value();
        m_transientBytes +=
            static_cast<u64>(physical.width) * physical.height * rhi::bytesPerTexel(physical.format);
    }
    m_allocated = true;
    m_allocatedWidth = m_width;
    m_allocatedHeight = m_height;
    m_allocatedWithAliasing = aliasing;
    return {};
}

rhi::TextureHandle FrameGraph::texture(TextureRef ref) const {
    if (ref.index >= m_textures.size()) {
        return {};
    }
    const Texture& texture = m_textures[ref.index];
    if (texture.imported) {
        return texture.handle;
    }
    return m_allocated && texture.firstPass ? m_physical[texture.physical].handle : rhi::TextureHandle{};
}

rhi::BufferHandle FrameGraph::buffer(BufferRef ref) const {
    return ref.index < m_buffers.size() ? m_buffers[ref.index].handle : rhi::BufferHandle{};
}

// ---------------------------------------------------------------------------
// Execution

bool FrameGraph::isPassEnabled(u32 pass) const {
    const Pass& p = m_passes.at(pass);
    return !p.hasToggle || p.enabled.get();
}

std::optional<f64> FrameGraph::passGpuMilliseconds(u32 pass) const {
    const Pass& p = m_passes.at(pass);
    return p.timer ? p.timer->lastFrameMilliseconds() : std::nullopt;
}

rhi::RenderPassDesc FrameGraph::renderPass(const Pass& pass, bool onlyClears) const {
    rhi::RenderPassDesc desc;
    const auto loadOf = [onlyClears](LoadOp load) { return onlyClears ? LoadOp::Clear : load; };
    if (pass.window) {
        desc.colorCount = 1;
        desc.colors[0] = {.swapchain = true, .load = loadOf(pass.windowLoad), .clearColor = pass.windowClear};
        return desc;
    }
    for (const TextureUse& use : pass.textures) {
        if (onlyClears && use.load == LoadOp::Load) {
            continue;
        }
        if (use.access == TextureAccess::ColorAttachment) {
            desc.colors[desc.colorCount++] = {.texture = texture(use.texture),
                                              .load = loadOf(use.load),
                                              .store = use.store,
                                              .clearColor = use.clearColor};
        } else if (use.access == TextureAccess::DepthAttachment) {
            desc.hasDepth = true;
            desc.depth = {.texture = texture(use.texture),
                          .load = loadOf(use.load),
                          .store = use.store,
                          .clearDepth = use.clearDepth};
        }
    }
    return desc;
}

Result<void> FrameGraph::execute(rhi::CommandList& commands) {
    if (!m_compiled) {
        return Error(ErrorCode::InvalidArgument, "Compile the frame graph before executing it");
    }
    const bool aliasing = m_settings == nullptr || m_aliasing.get();
    if (!m_allocated || m_width != m_allocatedWidth || m_height != m_allocatedHeight ||
        aliasing != m_allocatedWithAliasing) {
        VXE_TRY(allocate());
    }
    PassContext context{
        .device = m_device, .commands = commands, .graph = *this, .width = m_width, .height = m_height};
    u32 passesRun = 0;
    for (const Pass& pass : m_passes) {
        if (pass.barrier != BarrierScope::None) {
            commands.resourceBarrier(pass.barrier);
        }
        const bool enabled = !pass.hasToggle || pass.enabled.get();
        if (!enabled) {
            if (pass.clearsWhenDisabled) {
                commands.beginRenderPass(renderPass(pass, true));
                commands.endRenderPass();
            }
            if (m_stats != nullptr && pass.gpuTime.isValid()) {
                m_stats->set(pass.gpuTime, 0.0);
            }
            continue;
        }
        ++passesRun;
        commands.beginDebugGroup(pass.desc.name);
        if (pass.timer) {
            pass.timer->beginFrame(m_device, commands);
        }
        if (pass.desc.type == PassType::Raster) {
            commands.beginRenderPass(renderPass(pass, false));
            pass.desc.record(context);
            commands.endRenderPass();
        } else {
            pass.desc.record(context);
        }
        if (pass.timer) {
            pass.timer->endFrame(commands);
            if (m_stats != nullptr && pass.gpuTime.isValid()) {
                m_stats->set(pass.gpuTime, pass.timer->lastFrameMilliseconds().value_or(0.0));
            }
        }
        commands.endDebugGroup();
    }
    if (m_stats != nullptr) {
        m_stats->set(m_transientBytesStat, static_cast<f64>(m_transientBytes));
        m_stats->add(m_barriersStat, m_barrierCount);
        m_stats->add(m_passesStat, passesRun);
    }
    return {};
}

} // namespace vxe::render
