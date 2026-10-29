#pragma once

#include <vxe/core/assert.hpp>
#include <vxe/core/types.hpp>
#include <vxe/rhi/handles.hpp>
#include <vxe/rhi/types.hpp>

#include <cstring>
#include <iterator>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

// Command lists (Section 7.4).
//
// A command list is a flat byte buffer of small plain structs. Recording a
// command is a memcpy, which is why CommandList is not virtual: the cost of
// recording thousands of commands per frame is a few nanoseconds each. The
// backend walks the buffer on submit and translates each command. The same
// design lets a future Vulkan backend record on worker threads.
namespace vxe::rhi {

enum class CommandType : u16 {
    BeginRenderPass,
    EndRenderPass,
    BindPipeline,
    BindUniformBuffer,
    BindStorageBuffer,
    BindTexture,
    BindStorageImage,
    BindIndexBuffer,
    SetViewport,
    SetScissor,
    Draw,
    DrawIndexed,
    MultiDrawIndexedIndirect,
    MultiDrawIndexedIndirectCount,
    Dispatch,
    CopyBuffer,
    Barrier,
    BeginDebugGroup,
    EndDebugGroup,
    WriteTimestamp,
    Count,
};

// The payload of each command. All are trivially copyable, so they can be
// stored as raw bytes and read back with memcpy.
namespace cmd {

struct BeginRenderPass {
    RenderPassDesc pass;
};
struct EndRenderPass {};
struct BindPipeline {
    PipelineHandle pipeline;
};
struct BindBufferRange {
    u32 set = 0;
    u32 slot = 0;
    BufferHandle buffer;
    usize offset = 0;
    usize size = 0;
};
struct BindTexture {
    u32 set = 0;
    u32 slot = 0;
    TextureHandle texture;
    SamplerHandle sampler;
};
struct BindStorageImage {
    u32 set = 0;
    u32 slot = 0;
    TextureHandle texture;
    u32 mipLevel = 0;
    ImageAccess access = ImageAccess::ReadOnly;
};
struct BindIndexBuffer {
    BufferHandle buffer;
    IndexType type = IndexType::Uint32;
    usize offset = 0;
};
struct SetViewport {
    f32 x = 0.0F;
    f32 y = 0.0F;
    f32 width = 0.0F;
    f32 height = 0.0F;
};
struct SetScissor {
    i32 x = 0;
    i32 y = 0;
    u32 width = 0;
    u32 height = 0;
};
struct Draw {
    u32 vertexCount = 0;
    u32 instanceCount = 1;
    u32 firstVertex = 0;
    u32 firstInstance = 0;
};
struct DrawIndexed {
    u32 indexCount = 0;
    u32 instanceCount = 1;
    u32 firstIndex = 0;
    i32 vertexOffset = 0;
    u32 firstInstance = 0;
};
struct MultiDrawIndexedIndirect {
    BufferHandle commands;
    usize commandsOffset = 0;
    u32 drawCount = 0;
    u32 stride = 0; // 0 means tightly packed 20 byte commands.
};
struct MultiDrawIndexedIndirectCount {
    BufferHandle commands;
    usize commandsOffset = 0;
    BufferHandle count;
    usize countOffset = 0;
    u32 maxDraws = 0;
    u32 stride = 0;
};
struct Dispatch {
    u32 groupsX = 1;
    u32 groupsY = 1;
    u32 groupsZ = 1;
};
struct CopyBuffer {
    BufferHandle src;
    usize srcOffset = 0;
    BufferHandle dst;
    usize dstOffset = 0;
    usize size = 0;
};
struct Barrier {
    BarrierScope scope = BarrierScope::All;
};
// Followed in the buffer by `length` bytes of the name.
struct BeginDebugGroup {
    u32 length = 0;
};
struct EndDebugGroup {};
struct WriteTimestamp {
    QueryHandle query;
};

} // namespace cmd

// One recorded command as seen by a backend: its type and its payload bytes.
struct CommandView {
    CommandType type = CommandType::Count;
    std::span<const std::byte> payload;

    // Copies the payload into its struct. memcpy, because the bytes in the
    // buffer are not necessarily aligned for the struct.
    template <typename T>
    [[nodiscard]] T as() const noexcept {
        static_assert(std::is_trivially_copyable_v<T>);
        VXE_ASSERT(payload.size() >= sizeof(T), "Command payload too small");
        T value{};
        std::memcpy(&value, payload.data(), sizeof(T));
        return value;
    }

    // The name of a BeginDebugGroup command.
    [[nodiscard]] std::string_view debugGroupName() const noexcept {
        const auto header = as<cmd::BeginDebugGroup>();
        return {reinterpret_cast<const char*>(payload.data() + sizeof(cmd::BeginDebugGroup)), header.length};
    }
};

class CommandList {
public:
    void beginRenderPass(const RenderPassDesc& pass) {
        record(CommandType::BeginRenderPass, cmd::BeginRenderPass{pass});
    }
    void endRenderPass() { record(CommandType::EndRenderPass, cmd::EndRenderPass{}); }
    void bindPipeline(PipelineHandle pipeline) {
        record(CommandType::BindPipeline, cmd::BindPipeline{pipeline});
    }
    void bindUniformBuffer(u32 set, u32 slot, BufferHandle buffer, usize offset, usize size) {
        record(
            CommandType::BindUniformBuffer,
            cmd::BindBufferRange{.set = set, .slot = slot, .buffer = buffer, .offset = offset, .size = size});
    }
    void bindStorageBuffer(u32 set, u32 slot, BufferHandle buffer, usize offset, usize size) {
        record(
            CommandType::BindStorageBuffer,
            cmd::BindBufferRange{.set = set, .slot = slot, .buffer = buffer, .offset = offset, .size = size});
    }
    void bindTexture(u32 set, u32 slot, TextureHandle texture, SamplerHandle sampler) {
        record(CommandType::BindTexture,
               cmd::BindTexture{.set = set, .slot = slot, .texture = texture, .sampler = sampler});
    }
    // Binds one mip level of a texture created with TextureUsage::Storage as
    // a storage image, for compute shaders that read or write single texels
    // (imageLoad, imageStore), for example the levels of a depth pyramid.
    // Images have their own small binding space: OpenGL only guarantees 8
    // image units, so the OpenGL backend flattens (set, slot) as
    // set * 2 + slot, which allows slots 0 and 1 in sets 0 to 3. Shaders use
    // VXE_IMAGE_BINDING from bindings.glsl.
    void bindStorageImage(u32 set, u32 slot, TextureHandle texture, u32 mipLevel, ImageAccess access) {
        record(CommandType::BindStorageImage,
               cmd::BindStorageImage{
                   .set = set, .slot = slot, .texture = texture, .mipLevel = mipLevel, .access = access});
    }
    void bindIndexBuffer(BufferHandle buffer, IndexType type, usize offset = 0) {
        record(CommandType::BindIndexBuffer,
               cmd::BindIndexBuffer{.buffer = buffer, .type = type, .offset = offset});
    }
    void setViewport(f32 x, f32 y, f32 width, f32 height) {
        record(CommandType::SetViewport, cmd::SetViewport{.x = x, .y = y, .width = width, .height = height});
    }
    void setScissor(i32 x, i32 y, u32 width, u32 height) {
        record(CommandType::SetScissor, cmd::SetScissor{.x = x, .y = y, .width = width, .height = height});
    }
    // Draws without an index buffer. Chunk geometry uses this with vertex
    // pulling: the vertex shader builds positions from gl_VertexID (Section 8.2).
    void draw(u32 vertexCount, u32 instanceCount = 1, u32 firstVertex = 0, u32 firstInstance = 0) {
        record(CommandType::Draw, cmd::Draw{.vertexCount = vertexCount,
                                            .instanceCount = instanceCount,
                                            .firstVertex = firstVertex,
                                            .firstInstance = firstInstance});
    }
    void drawIndexed(u32 indexCount, u32 instanceCount = 1, u32 firstIndex = 0, i32 vertexOffset = 0,
                     u32 firstInstance = 0) {
        record(CommandType::DrawIndexed, cmd::DrawIndexed{.indexCount = indexCount,
                                                          .instanceCount = instanceCount,
                                                          .firstIndex = firstIndex,
                                                          .vertexOffset = vertexOffset,
                                                          .firstInstance = firstInstance});
    }
    void multiDrawIndexedIndirect(BufferHandle commands, usize commandsOffset, u32 drawCount,
                                  u32 stride = 0) {
        record(CommandType::MultiDrawIndexedIndirect,
               cmd::MultiDrawIndexedIndirect{.commands = commands,
                                             .commandsOffset = commandsOffset,
                                             .drawCount = drawCount,
                                             .stride = stride});
    }
    // The draw count is read from a GPU buffer written by a culling compute
    // shader (Section 8.3), so the CPU never waits for the GPU's result.
    void multiDrawIndexedIndirectCount(BufferHandle commands, usize commandsOffset, BufferHandle count,
                                       usize countOffset, u32 maxDraws, u32 stride = 0) {
        record(CommandType::MultiDrawIndexedIndirectCount,
               cmd::MultiDrawIndexedIndirectCount{.commands = commands,
                                                  .commandsOffset = commandsOffset,
                                                  .count = count,
                                                  .countOffset = countOffset,
                                                  .maxDraws = maxDraws,
                                                  .stride = stride});
    }
    void dispatch(u32 groupsX, u32 groupsY = 1, u32 groupsZ = 1) {
        record(CommandType::Dispatch,
               cmd::Dispatch{.groupsX = groupsX, .groupsY = groupsY, .groupsZ = groupsZ});
    }
    void copyBuffer(BufferHandle src, usize srcOffset, BufferHandle dst, usize dstOffset, usize size) {
        record(CommandType::CopyBuffer,
               cmd::CopyBuffer{
                   .src = src, .srcOffset = srcOffset, .dst = dst, .dstOffset = dstOffset, .size = size});
    }
    // Declares that later commands read what earlier commands wrote.
    void resourceBarrier(BarrierScope scope) { record(CommandType::Barrier, cmd::Barrier{scope}); }
    void beginDebugGroup(std::string_view name);
    void endDebugGroup() { record(CommandType::EndDebugGroup, cmd::EndDebugGroup{}); }
    void writeTimestamp(QueryHandle query) {
        record(CommandType::WriteTimestamp, cmd::WriteTimestamp{query});
    }

    // Forgets every command but keeps the memory, so a list reused every
    // frame stops allocating after the first few frames.
    void reset() noexcept {
        m_bytes.clear();
        m_count = 0;
    }

    [[nodiscard]] usize commandCount() const noexcept { return m_count; }
    [[nodiscard]] usize sizeBytes() const noexcept { return m_bytes.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_count == 0; }

    // Walks the recorded commands in order. Used by backends on submit.
    class Iterator {
    public:
        using iterator_category = std::forward_iterator_tag; // NOLINT(readability-identifier-naming)
        using value_type = CommandView;                      // NOLINT(readability-identifier-naming)
        using difference_type = std::ptrdiff_t;              // NOLINT(readability-identifier-naming)

        Iterator() = default;
        Iterator(std::span<const std::byte> bytes, usize offset) noexcept
            : m_bytes(bytes)
            , m_offset(offset) {}

        [[nodiscard]] CommandView operator*() const noexcept;
        Iterator& operator++() noexcept;
        Iterator operator++(int) noexcept {
            Iterator copy = *this;
            ++*this;
            return copy;
        }
        friend bool operator==(const Iterator& a, const Iterator& b) noexcept {
            return a.m_offset == b.m_offset;
        }

    private:
        std::span<const std::byte> m_bytes;
        usize m_offset = 0;
    };

    [[nodiscard]] Iterator begin() const noexcept { return {m_bytes, 0}; }
    [[nodiscard]] Iterator end() const noexcept { return {m_bytes, m_bytes.size()}; }

private:
    // Every command starts with this header; `size` counts only the payload.
    struct Header {
        CommandType type;
        u16 reserved = 0;
        u32 size = 0;
    };

    template <typename T>
    void record(CommandType type, const T& payload) {
        static_assert(std::is_trivially_copyable_v<T>, "Commands must be plain data");
        appendHeader(type, sizeof(T));
        append(&payload, sizeof(T));
    }
    void appendHeader(CommandType type, usize payloadSize);
    void append(const void* data, usize size);

    std::vector<std::byte> m_bytes; // Reused every frame, so it stops allocating after warm up.
    usize m_count = 0;

    friend class Iterator;
};

} // namespace vxe::rhi
