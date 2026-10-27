#pragma once

// The backend objects behind RHI handles. Private to the OpenGL backend.

#include <vxe/core/types.hpp>
#include <vxe/rhi/handles.hpp>
#include <vxe/rhi/types.hpp>

#include <glad/gl.h>

#include <array>
#include <string>

namespace vxe::rhi::gl {

struct Buffer {
    GLuint id = 0;
    usize size = 0;
    BufferUsage usage = BufferUsage::None;
    std::byte* mapped = nullptr; // Persistent mapping of CpuWrite buffers, valid until deletion.
    std::string name;
};

struct Texture {
    GLuint id = 0;
    TextureDesc desc; // debugName cleared; `name` owns the text.
    std::string name;
    usize bytes = 0;
};

struct Sampler {
    GLuint id = 0;
    std::string name;
};

struct Fence {
    GLsync sync = nullptr;
};

struct Shader {
    GLuint id = 0;
    ShaderStage stage = ShaderStage::Vertex;
    std::string name;
};

// A linked program plus every piece of fixed function state, applied as a
// whole on bind (through the StateCache, which skips what is already set).
struct Pipeline {
    GLuint program = 0;
    bool compute = false;
    GLenum topology = GL_TRIANGLES;
    RasterState raster;
    DepthState depth;
    std::array<BlendState, kMaxColorAttachments> blend{};
    std::string name;
};

struct Query {
    GLuint id = 0;
    bool written = false; // OpenGL results are undefined for queries never written.
};

// A destroyed object waiting for the GPU to finish the frames that may use
// it (DeferredReleaseQueue). Only the kind and the GL name are needed.
struct DeferredObject {
    enum class Kind : u8 {
        Buffer,
        Texture,
        Sampler,
        Shader,
        Program
    };
    Kind kind = Kind::Buffer;
    GLuint id = 0;
};

// Every object the device owns, by handle. The device creates and destroys
// them; the command replayer only looks them up.
struct Resources {
    HandlePool<BufferTag, Buffer> buffers;
    HandlePool<TextureTag, Texture> textures;
    HandlePool<SamplerTag, Sampler> samplers;
    HandlePool<ShaderTag, Shader> shaders;
    HandlePool<PipelineTag, Pipeline> pipelines;
    HandlePool<FenceTag, Fence> fences;
    HandlePool<QueryTag, Query> queries;
};

} // namespace vxe::rhi::gl
