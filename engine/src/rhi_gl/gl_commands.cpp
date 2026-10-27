#include "gl_commands.hpp"

#include "gl_formats.hpp"

#include <vxe/platform/window.hpp>

#include <algorithm>
#include <cstdint>
#include <span>

namespace vxe::rhi::gl {
namespace {

// Size of one DrawElementsIndirectCommand: count, instanceCount, firstIndex,
// baseVertex, baseInstance, five 32 bit values.
constexpr u32 kIndexedIndirectCommandSize = 20;

// Bindings are flattened as set * 16 + slot (engine/shaders/include/bindings.glsl).
constexpr u32 kSlotsPerSet = 16;

// OpenGL takes offsets into bound buffers as pointers, a leftover from client
// side arrays. This is the one place that turns an offset into one.
[[nodiscard]] const void* offsetPointer(usize offset) noexcept {
    // NOLINTNEXTLINE(performance-no-int-to-ptr): the GL API defines offsets this way.
    return reinterpret_cast<const void*>(static_cast<std::uintptr_t>(offset));
}

[[nodiscard]] GLbitfield barrierBits(BarrierScope scope) noexcept {
    if (scope == BarrierScope::All) {
        return GL_ALL_BARRIER_BITS;
    }
    GLbitfield bits = 0;
    const auto add = [&bits, scope](BarrierScope flag, GLbitfield glBits) {
        if (hasFlag(scope, flag)) {
            bits |= glBits;
        }
    };
    add(BarrierScope::VertexData, GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);
    add(BarrierScope::IndexData, GL_ELEMENT_ARRAY_BARRIER_BIT);
    add(BarrierScope::UniformData, GL_UNIFORM_BARRIER_BIT);
    add(BarrierScope::StorageData, GL_SHADER_STORAGE_BARRIER_BIT);
    add(BarrierScope::IndirectCommands, GL_COMMAND_BARRIER_BIT);
    add(BarrierScope::TextureFetch, GL_TEXTURE_FETCH_BARRIER_BIT);
    add(BarrierScope::StorageImage, GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    // CPU reads (readBuffer), copies and texture uploads after shader writes.
    add(BarrierScope::TransferData, GL_BUFFER_UPDATE_BARRIER_BIT | GL_PIXEL_BUFFER_BARRIER_BIT |
                                        GL_TEXTURE_UPDATE_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
    return bits;
}

[[nodiscard]] bool isIntegerFormat(Format format) noexcept {
    return format == Format::R32Uint || format == Format::RG32Uint || format == Format::RGBA32Uint;
}

[[nodiscard]] u32 glLimit(GLenum name) {
    GLint value = 0;
    glGetIntegerv(name, &value);
    return static_cast<u32>(std::max(value, 0));
}

} // namespace

void CommandReplayer::initialize() {
    // Core profile OpenGL draws nothing without a vertex array object. The
    // engine pulls vertices from storage buffers (Section 8.2), so one empty
    // VAO, holding only the index buffer, serves every draw.
    glCreateVertexArrays(1, &m_vertexArray);
    glBindVertexArray(m_vertexArray);

    m_limits.uniformBufferBindings = glLimit(GL_MAX_UNIFORM_BUFFER_BINDINGS);
    m_limits.storageBufferBindings = glLimit(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS);
    m_limits.textureUnits = glLimit(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS);
    for (u32 axis = 0; axis < 3; ++axis) {
        GLint groups = 0;
        glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, axis, &groups);
        m_limits.computeGroups[axis] = static_cast<u32>(std::max(groups, 0));
    }
}

void CommandReplayer::shutdown() {
    for (const CachedFramebuffer& cached : m_framebuffers) {
        glDeleteFramebuffers(1, &cached.framebuffer);
    }
    m_framebuffers.clear();
    if (m_vertexArray != 0) {
        glBindVertexArray(0);
        glDeleteVertexArrays(1, &m_vertexArray);
        m_vertexArray = 0;
    }
}

void CommandReplayer::submit(const CommandList& commands) {
    for (const CommandView command : commands) {
        ++m_stats.commands;
        if (!replay(command)) {
            ++m_stats.invalidCommands;
        }
    }
    // A list must leave no pass open and no debug group pushed. Close them,
    // so the next list starts clean, and count the mistake.
    if (m_insidePass) {
        ++m_stats.invalidCommands;
        static_cast<void>(endRenderPass());
    }
    if (m_debugGroupDepth > 0) {
        ++m_stats.invalidCommands;
        while (m_debugGroupDepth > 0) {
            static_cast<void>(endDebugGroup());
        }
    }
}

void CommandReplayer::onPipelineDestroyed(PipelineHandle handle) noexcept {
    if (handle == m_boundPipeline) {
        m_boundPipeline = {};
    }
}

void CommandReplayer::onTextureDestroyed(GLuint texture) {
    // Framebuffers that use the texture would point at a dead object, and
    // OpenGL may reuse the name for a new texture; drop them.
    std::erase_if(m_framebuffers, [texture](const CachedFramebuffer& cached) {
        const auto usedColors = std::span(cached.key.colors).first(cached.key.colorCount);
        const bool uses =
            cached.key.depth == texture || std::ranges::find(usedColors, texture) != usedColors.end();
        if (uses) {
            glDeleteFramebuffers(1, &cached.framebuffer);
        }
        return uses;
    });
}

bool CommandReplayer::replay(const CommandView& command) {
    switch (command.type) {
    case CommandType::BeginRenderPass:
        return beginRenderPass(command.as<cmd::BeginRenderPass>().pass);
    case CommandType::EndRenderPass:
        return endRenderPass();
    case CommandType::BindPipeline:
        return bindPipeline(command.as<cmd::BindPipeline>().pipeline);
    case CommandType::BindUniformBuffer:
        return bindBufferRange(GL_UNIFORM_BUFFER, BufferUsage::Uniform,
                               m_capabilities.uniformBufferOffsetAlignment, m_limits.uniformBufferBindings,
                               command.as<cmd::BindBufferRange>());
    case CommandType::BindStorageBuffer:
        return bindBufferRange(GL_SHADER_STORAGE_BUFFER, BufferUsage::Storage,
                               m_capabilities.storageBufferOffsetAlignment, m_limits.storageBufferBindings,
                               command.as<cmd::BindBufferRange>());
    case CommandType::BindTexture:
        return bindTexture(command.as<cmd::BindTexture>());
    case CommandType::BindIndexBuffer:
        return bindIndexBuffer(command.as<cmd::BindIndexBuffer>());
    case CommandType::SetViewport:
        return setViewport(command.as<cmd::SetViewport>());
    case CommandType::SetScissor:
        return setScissor(command.as<cmd::SetScissor>());
    case CommandType::Draw:
        return draw(command.as<cmd::Draw>());
    case CommandType::DrawIndexed:
        return drawIndexed(command.as<cmd::DrawIndexed>());
    case CommandType::MultiDrawIndexedIndirect:
        return multiDrawIndirect(command.as<cmd::MultiDrawIndexedIndirect>());
    case CommandType::MultiDrawIndexedIndirectCount:
        return multiDrawIndirectCount(command.as<cmd::MultiDrawIndexedIndirectCount>());
    case CommandType::Dispatch:
        return dispatch(command.as<cmd::Dispatch>());
    case CommandType::CopyBuffer:
        return copyBuffer(command.as<cmd::CopyBuffer>());
    case CommandType::Barrier:
        return barrier(command.as<cmd::Barrier>());
    case CommandType::BeginDebugGroup:
        return beginDebugGroup(command);
    case CommandType::EndDebugGroup:
        return endDebugGroup();
    case CommandType::WriteTimestamp:
        return writeTimestamp(command.as<cmd::WriteTimestamp>());
    case CommandType::Count:
        break;
    }
    return false;
}

std::optional<GLuint> CommandReplayer::framebufferFor(const RenderPassDesc& pass) {
    if (pass.colorCount > kMaxColorAttachments) {
        return std::nullopt;
    }
    const bool swapchain = pass.colorCount == 1 && pass.colors[0].swapchain;
    if (swapchain) {
        // The window's back buffer has no depth buffer of its own (the window
        // asks for none) and OpenGL cannot attach textures to it, so a window
        // pass has exactly one color attachment and no depth.
        return pass.hasDepth ? std::nullopt : std::optional<GLuint>(0);
    }

    FramebufferKey key;
    key.colorCount = pass.colorCount;
    u32 width = 0;
    u32 height = 0;
    const auto matchesSize = [&width, &height](const TextureDesc& desc) {
        if (width == 0) {
            width = desc.width;
            height = desc.height;
        }
        return desc.width == width && desc.height == height && desc.type == TextureType::Texture2D;
    };
    for (u32 i = 0; i < pass.colorCount; ++i) {
        const Texture* texture = m_resources.textures.get(pass.colors[i].texture);
        if (pass.colors[i].swapchain || texture == nullptr ||
            !hasFlag(texture->desc.usage, TextureUsage::RenderTarget) || !matchesSize(texture->desc)) {
            return std::nullopt;
        }
        key.colors[i] = texture->id;
    }
    if (pass.hasDepth) {
        const Texture* depth = m_resources.textures.get(pass.depth.texture);
        if (depth == nullptr || !hasFlag(depth->desc.usage, TextureUsage::DepthTarget) ||
            !matchesSize(depth->desc)) {
            return std::nullopt;
        }
        key.depth = depth->id;
        key.depthStencil = depth->desc.format == Format::D24UnormS8Uint;
    }
    if (key.colorCount == 0 && key.depth == 0) {
        return std::nullopt;
    }

    const auto cached = std::ranges::find(m_framebuffers, key, &CachedFramebuffer::key);
    if (cached != m_framebuffers.end()) {
        return cached->framebuffer;
    }
    GLuint framebuffer = 0;
    glCreateFramebuffers(1, &framebuffer);
    std::array<GLenum, kMaxColorAttachments> drawBuffers{};
    for (u32 i = 0; i < key.colorCount; ++i) {
        glNamedFramebufferTexture(framebuffer, GL_COLOR_ATTACHMENT0 + i, key.colors[i], 0);
        drawBuffers[i] = GL_COLOR_ATTACHMENT0 + i;
    }
    if (key.colorCount > 0) {
        glNamedFramebufferDrawBuffers(framebuffer, static_cast<GLsizei>(key.colorCount), drawBuffers.data());
    } else {
        glNamedFramebufferDrawBuffer(framebuffer, GL_NONE); // Depth only, for shadow style passes.
    }
    if (key.depth != 0) {
        glNamedFramebufferTexture(
            framebuffer, key.depthStencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT, key.depth, 0);
    }
    if (glCheckNamedFramebufferStatus(framebuffer, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteFramebuffers(1, &framebuffer);
        return std::nullopt;
    }
    m_framebuffers.push_back({.key = key, .framebuffer = framebuffer});
    return framebuffer;
}

bool CommandReplayer::beginRenderPass(const RenderPassDesc& pass) {
    if (m_insidePass) {
        return false;
    }
    const auto framebuffer = framebufferFor(pass);
    if (!framebuffer) {
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, *framebuffer);

    // Offscreen sRGB targets encode on write. The window shows values as
    // written; turning sRGB on there would brighten it on drivers that treat
    // the back buffer as sRGB capable. Post processing (E8.4) owns that step.
    const bool srgb = *framebuffer != 0;
    if (m_srgbWrites != srgb) {
        if (srgb) {
            glEnable(GL_FRAMEBUFFER_SRGB);
        } else {
            glDisable(GL_FRAMEBUFFER_SRGB);
        }
        m_srgbWrites = srgb;
    }

    // The render area: the given rectangle, or the whole target.
    u32 width = pass.width;
    u32 height = pass.height;
    if (width == 0 || height == 0) {
        if (*framebuffer == 0) {
            width = static_cast<u32>(std::max(m_window.pixelSize().x, 0));
            height = static_cast<u32>(std::max(m_window.pixelSize().y, 0));
        } else {
            const TextureHandle first = pass.colorCount > 0 ? pass.colors[0].texture : pass.depth.texture;
            const Texture* texture = m_resources.textures.get(first);
            width = texture->desc.width;
            height = texture->desc.height;
        }
    }
    glViewport(pass.x, pass.y, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    setScissorEnabled(false); // Scissors also clip clears, so a pass starts without one.

    invalidateAttachments(*framebuffer, pass, false);
    clearAttachments(*framebuffer, pass);
    m_insidePass = true;
    m_pass = pass;
    m_framebuffer = *framebuffer;
    return true;
}

void CommandReplayer::clearAttachments(GLuint framebuffer, const RenderPassDesc& pass) {
    for (u32 i = 0; i < pass.colorCount; ++i) {
        const ColorAttachment& color = pass.colors[i];
        if (color.load != LoadOp::Clear) {
            continue;
        }
        const Texture* texture = color.swapchain ? nullptr : m_resources.textures.get(color.texture);
        const auto drawBuffer = static_cast<GLint>(i);
        if (texture != nullptr && isIntegerFormat(texture->desc.format)) {
            const std::array<GLuint, 4> value{
                static_cast<GLuint>(color.clearColor[0]), static_cast<GLuint>(color.clearColor[1]),
                static_cast<GLuint>(color.clearColor[2]), static_cast<GLuint>(color.clearColor[3])};
            glClearNamedFramebufferuiv(framebuffer, GL_COLOR, drawBuffer, value.data());
        } else {
            glClearNamedFramebufferfv(framebuffer, GL_COLOR, drawBuffer, color.clearColor.data());
        }
    }
    if (pass.hasDepth && pass.depth.load == LoadOp::Clear) {
        // Depth clears obey the depth write mask, which the last pipeline may
        // have turned off (translucent passes do).
        m_state.enableDepthWrites();
        const Texture* depth = m_resources.textures.get(pass.depth.texture);
        if (depth != nullptr && depth->desc.format == Format::D24UnormS8Uint) {
            glClearNamedFramebufferfi(framebuffer, GL_DEPTH_STENCIL, 0, pass.depth.clearDepth, 0);
        } else {
            glClearNamedFramebufferfv(framebuffer, GL_DEPTH, 0, &pass.depth.clearDepth);
        }
    }
}

// Tells the driver which attachment contents do not matter: at the start for
// LoadOp::DontCare, at the end for StoreOp::DontCare. Tiled and bandwidth
// limited GPUs then skip loading or writing those pixels.
void CommandReplayer::invalidateAttachments(GLuint framebuffer, const RenderPassDesc& pass, bool atEnd) {
    if (framebuffer == 0) {
        return; // The back buffer is always presented, so it is never discarded.
    }
    std::array<GLenum, kMaxColorAttachments + 1> attachments{};
    GLsizei count = 0;
    for (u32 i = 0; i < pass.colorCount; ++i) {
        const bool discard =
            atEnd ? pass.colors[i].store == StoreOp::DontCare : pass.colors[i].load == LoadOp::DontCare;
        if (discard) {
            attachments[static_cast<usize>(count++)] = GL_COLOR_ATTACHMENT0 + i;
        }
    }
    if (pass.hasDepth) {
        const bool discard =
            atEnd ? pass.depth.store == StoreOp::DontCare : pass.depth.load == LoadOp::DontCare;
        if (discard) {
            const Texture* depth = m_resources.textures.get(pass.depth.texture);
            const bool stencil = depth != nullptr && depth->desc.format == Format::D24UnormS8Uint;
            attachments[static_cast<usize>(count++)] =
                stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
        }
    }
    if (count > 0) {
        glInvalidateNamedFramebufferData(framebuffer, count, attachments.data());
    }
}

bool CommandReplayer::endRenderPass() {
    if (!m_insidePass) {
        return false;
    }
    invalidateAttachments(m_framebuffer, m_pass, true);
    m_insidePass = false;
    return true;
}

bool CommandReplayer::bindPipeline(PipelineHandle handle) {
    ++m_stats.pipelineBinds;
    const Pipeline* pipeline = m_resources.pipelines.get(handle);
    if (pipeline == nullptr) {
        return false;
    }
    if (handle == m_boundPipeline) {
        ++m_stats.pipelineBindsSkipped;
        return true;
    }
    m_state.useProgram(pipeline->program);
    if (!pipeline->compute) {
        m_state.applyRaster(pipeline->raster);
        m_state.applyDepth(pipeline->depth);
        m_state.applyBlend(pipeline->blend);
    }
    m_boundPipeline = handle;
    m_pipelineIsCompute = pipeline->compute;
    m_topology = pipeline->topology;
    return true;
}

bool CommandReplayer::bindBufferRange(GLenum target, BufferUsage requiredUsage, u32 alignment,
                                      u32 maxBindings, const cmd::BindBufferRange& bind) {
    const Buffer* buffer = m_resources.buffers.get(bind.buffer);
    const u32 binding = bind.set * kSlotsPerSet + bind.slot;
    if (buffer == nullptr || !hasFlag(buffer->usage, requiredUsage) || bind.slot >= kSlotsPerSet ||
        binding >= maxBindings || bind.offset >= buffer->size ||
        (alignment > 0 && bind.offset % alignment != 0)) {
        return false;
    }
    // Size 0 means "from the offset to the end".
    const usize size = bind.size == 0 ? buffer->size - bind.offset : bind.size;
    if (size > buffer->size - bind.offset) {
        return false;
    }
    glBindBufferRange(target, binding, buffer->id, static_cast<GLintptr>(bind.offset),
                      static_cast<GLsizeiptr>(size));
    return true;
}

bool CommandReplayer::bindTexture(const cmd::BindTexture& bind) {
    const Texture* texture = m_resources.textures.get(bind.texture);
    const Sampler* sampler = m_resources.samplers.get(bind.sampler);
    const u32 unit = bind.set * kSlotsPerSet + bind.slot;
    if (texture == nullptr || sampler == nullptr || !hasFlag(texture->desc.usage, TextureUsage::Sampled) ||
        bind.slot >= kSlotsPerSet || unit >= m_limits.textureUnits) {
        return false;
    }
    glBindTextureUnit(unit, texture->id);
    glBindSampler(unit, sampler->id);
    return true;
}

bool CommandReplayer::bindIndexBuffer(const cmd::BindIndexBuffer& bind) {
    const Buffer* buffer = m_resources.buffers.get(bind.buffer);
    const u32 indexSize = bind.type == IndexType::Uint16 ? 2U : 4U;
    if (buffer == nullptr || !hasFlag(buffer->usage, BufferUsage::Index) || bind.offset >= buffer->size ||
        bind.offset % indexSize != 0) {
        return false;
    }
    glVertexArrayElementBuffer(m_vertexArray, buffer->id);
    m_indexBuffer = IndexBinding{.type = bind.type == IndexType::Uint16 ? GLenum{GL_UNSIGNED_SHORT}
                                                                        : GLenum{GL_UNSIGNED_INT},
                                 .indexSize = indexSize,
                                 .offset = bind.offset};
    return true;
}

bool CommandReplayer::setViewport(const cmd::SetViewport& viewport) const {
    if (!m_insidePass || viewport.width <= 0.0F || viewport.height <= 0.0F) {
        return false;
    }
    glViewportIndexedf(0, viewport.x, viewport.y, viewport.width, viewport.height);
    return true;
}

bool CommandReplayer::setScissor(const cmd::SetScissor& scissor) {
    if (!m_insidePass) {
        return false;
    }
    setScissorEnabled(true);
    glScissorIndexed(0, scissor.x, scissor.y, static_cast<GLsizei>(scissor.width),
                     static_cast<GLsizei>(scissor.height));
    return true;
}

void CommandReplayer::setScissorEnabled(bool enabled) {
    if (m_scissorEnabled != enabled) {
        if (enabled) {
            glEnable(GL_SCISSOR_TEST);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
        m_scissorEnabled = enabled;
    }
}

bool CommandReplayer::canDraw() const noexcept {
    return m_insidePass && m_resources.pipelines.isLive(m_boundPipeline) && !m_pipelineIsCompute;
}

bool CommandReplayer::draw(const cmd::Draw& draw) {
    if (!canDraw()) {
        return false;
    }
    glDrawArraysInstancedBaseInstance(m_topology, static_cast<GLint>(draw.firstVertex),
                                      static_cast<GLsizei>(draw.vertexCount),
                                      static_cast<GLsizei>(draw.instanceCount), draw.firstInstance);
    ++m_stats.drawCalls;
    return true;
}

bool CommandReplayer::drawIndexed(const cmd::DrawIndexed& draw) {
    if (!canDraw() || !m_indexBuffer) {
        return false;
    }
    const usize offset =
        m_indexBuffer->offset + static_cast<usize>(draw.firstIndex) * m_indexBuffer->indexSize;
    glDrawElementsInstancedBaseVertexBaseInstance(
        m_topology, static_cast<GLsizei>(draw.indexCount), m_indexBuffer->type, offsetPointer(offset),
        static_cast<GLsizei>(draw.instanceCount), draw.vertexOffset, draw.firstInstance);
    ++m_stats.drawCalls;
    return true;
}

bool CommandReplayer::isIndirectRangeValid(BufferHandle handle, usize offset, u32 drawCount,
                                           u32 stride) const {
    const Buffer* buffer = m_resources.buffers.get(handle);
    if (buffer == nullptr || !hasFlag(buffer->usage, BufferUsage::Indirect) || offset % 4 != 0 ||
        drawCount == 0) {
        return false;
    }
    const usize needed = static_cast<usize>(drawCount - 1) * stride + kIndexedIndirectCommandSize;
    return offset <= buffer->size && needed <= buffer->size - offset;
}

void CommandReplayer::bindIndirectBuffer(GLenum target, GLuint buffer, std::optional<GLuint>& bound) {
    if (bound != buffer) {
        glBindBuffer(target, buffer);
        bound = buffer;
    }
}

bool CommandReplayer::multiDrawIndirect(const cmd::MultiDrawIndexedIndirect& draw) {
    const u32 stride = draw.stride == 0 ? kIndexedIndirectCommandSize : draw.stride;
    if (!canDraw() || !m_indexBuffer || stride < kIndexedIndirectCommandSize || stride % 4 != 0 ||
        !isIndirectRangeValid(draw.commands, draw.commandsOffset, draw.drawCount, stride)) {
        return false;
    }
    bindIndirectBuffer(GL_DRAW_INDIRECT_BUFFER, m_resources.buffers.get(draw.commands)->id,
                       m_drawIndirectBuffer);
    // The index buffer offset is not applied here: indirect commands carry
    // their own firstIndex, relative to the start of the index buffer.
    glMultiDrawElementsIndirect(m_topology, m_indexBuffer->type, offsetPointer(draw.commandsOffset),
                                static_cast<GLsizei>(draw.drawCount), static_cast<GLsizei>(stride));
    ++m_stats.drawCalls;
    return true;
}

bool CommandReplayer::multiDrawIndirectCount(const cmd::MultiDrawIndexedIndirectCount& draw) {
    const u32 stride = draw.stride == 0 ? kIndexedIndirectCommandSize : draw.stride;
    const Buffer* count = m_resources.buffers.get(draw.count);
    if (!m_capabilities.multiDrawIndirectCount || !canDraw() || !m_indexBuffer ||
        stride < kIndexedIndirectCommandSize || stride % 4 != 0 ||
        !isIndirectRangeValid(draw.commands, draw.commandsOffset, draw.maxDraws, stride) ||
        count == nullptr || !hasFlag(count->usage, BufferUsage::Indirect) || draw.countOffset % 4 != 0 ||
        draw.countOffset + 4 > count->size) {
        return false;
    }
    bindIndirectBuffer(GL_DRAW_INDIRECT_BUFFER, m_resources.buffers.get(draw.commands)->id,
                       m_drawIndirectBuffer);
    bindIndirectBuffer(GL_PARAMETER_BUFFER, count->id, m_parameterBuffer);
    // Core in 4.6, otherwise the ARB extension with the same behavior.
    if (GLAD_GL_VERSION_4_6 != 0) {
        glMultiDrawElementsIndirectCount(m_topology, m_indexBuffer->type, offsetPointer(draw.commandsOffset),
                                         static_cast<GLintptr>(draw.countOffset),
                                         static_cast<GLsizei>(draw.maxDraws), static_cast<GLsizei>(stride));
    } else {
        glMultiDrawElementsIndirectCountARB(
            m_topology, m_indexBuffer->type, offsetPointer(draw.commandsOffset),
            static_cast<GLintptr>(draw.countOffset), static_cast<GLsizei>(draw.maxDraws),
            static_cast<GLsizei>(stride));
    }
    ++m_stats.drawCalls;
    return true;
}

bool CommandReplayer::dispatch(const cmd::Dispatch& dispatch) {
    const bool computeBound = m_resources.pipelines.isLive(m_boundPipeline) && m_pipelineIsCompute;
    if (m_insidePass || !computeBound || dispatch.groupsX == 0 || dispatch.groupsY == 0 ||
        dispatch.groupsZ == 0 || dispatch.groupsX > m_limits.computeGroups[0] ||
        dispatch.groupsY > m_limits.computeGroups[1] || dispatch.groupsZ > m_limits.computeGroups[2]) {
        return false;
    }
    glDispatchCompute(dispatch.groupsX, dispatch.groupsY, dispatch.groupsZ);
    ++m_stats.dispatches;
    return true;
}

bool CommandReplayer::copyBuffer(const cmd::CopyBuffer& copy) {
    const Buffer* src = m_resources.buffers.get(copy.src);
    const Buffer* dst = m_resources.buffers.get(copy.dst);
    if (m_insidePass || src == nullptr || dst == nullptr || !hasFlag(src->usage, BufferUsage::TransferSrc) ||
        !hasFlag(dst->usage, BufferUsage::TransferDst) || copy.size == 0 || copy.srcOffset > src->size ||
        copy.size > src->size - copy.srcOffset || copy.dstOffset > dst->size ||
        copy.size > dst->size - copy.dstOffset) {
        return false;
    }
    if (src->id == dst->id && copy.srcOffset < copy.dstOffset + copy.size &&
        copy.dstOffset < copy.srcOffset + copy.size) {
        return false; // OpenGL refuses overlapping copies within one buffer.
    }
    glCopyNamedBufferSubData(src->id, dst->id, static_cast<GLintptr>(copy.srcOffset),
                             static_cast<GLintptr>(copy.dstOffset), static_cast<GLsizeiptr>(copy.size));
    return true;
}

bool CommandReplayer::barrier(const cmd::Barrier& barrier) {
    const GLbitfield bits = barrierBits(barrier.scope);
    if (bits != 0) {
        glMemoryBarrier(bits);
    }
    return true;
}

bool CommandReplayer::beginDebugGroup(const CommandView& command) {
    const std::string_view name = command.debugGroupName();
    glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, static_cast<GLsizei>(name.size()), name.data());
    ++m_debugGroupDepth;
    return true;
}

bool CommandReplayer::endDebugGroup() {
    if (m_debugGroupDepth == 0) {
        return false;
    }
    glPopDebugGroup();
    --m_debugGroupDepth;
    return true;
}

bool CommandReplayer::writeTimestamp(const cmd::WriteTimestamp& timestamp) {
    Query* query = m_resources.queries.get(timestamp.query);
    if (query == nullptr) {
        return false;
    }
    glQueryCounter(query->id, GL_TIMESTAMP);
    query->written = true;
    return true;
}

} // namespace vxe::rhi::gl
