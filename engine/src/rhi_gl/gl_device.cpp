#include "vxe/rhi_gl/gl_device.hpp"

#include "gl_commands.hpp"
#include "gl_formats.hpp"
#include "gl_resources.hpp"
#include "gl_state.hpp"

#include <vxe/core/log.hpp>
#include <vxe/platform/window.hpp>
#include <vxe/rhi/deferred_release.hpp>
#include <vxe/rhi/shader_source.hpp>
#include <vxe/rhi/validation.hpp>

#include <glad/gl.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <deque>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace vxe::rhi {
namespace {

// Reads a shader or program info log, which OpenGL keeps per object.
template <typename GetLength, typename GetLog>
std::string infoLog(GLuint object, GetLength getLength, GetLog getLog) {
    GLint length = 0;
    getLength(object, GL_INFO_LOG_LENGTH, &length);
    std::string log(static_cast<usize>(std::max(length, 0)), '\0');
    if (length > 0) {
        GLsizei written = 0;
        getLog(object, length, &written, log.data());
        log.resize(static_cast<usize>(std::max(written, 0)));
    }
    return log;
}

std::string_view glString(GLenum name) {
    const GLubyte* value = glGetString(name);
    return value == nullptr ? std::string_view() : std::string_view(reinterpret_cast<const char*>(value));
}

// glObjectLabel takes a length and a pointer, so names need no terminator.
void label(GLenum type, GLuint id, std::string_view name) {
    if (!name.empty()) {
        glObjectLabel(type, id, static_cast<GLsizei>(name.size()), name.data());
    }
}

u32 glLimit(GLenum name) {
    GLint value = 0;
    glGetIntegerv(name, &value);
    return static_cast<u32>(std::max(value, 0));
}

std::string_view debugSourceName(GLenum source) {
    switch (source) {
    case GL_DEBUG_SOURCE_API:
        return "api";
    case GL_DEBUG_SOURCE_WINDOW_SYSTEM:
        return "window system";
    case GL_DEBUG_SOURCE_SHADER_COMPILER:
        return "shader compiler";
    case GL_DEBUG_SOURCE_THIRD_PARTY:
        return "third party";
    case GL_DEBUG_SOURCE_APPLICATION:
        return "application";
    default:
        return "other";
    }
}

std::string_view debugTypeName(GLenum type) {
    switch (type) {
    case GL_DEBUG_TYPE_ERROR:
        return "error";
    case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR:
        return "deprecated";
    case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:
        return "undefined behavior";
    case GL_DEBUG_TYPE_PORTABILITY:
        return "portability";
    case GL_DEBUG_TYPE_PERFORMANCE:
        return "performance";
    case GL_DEBUG_TYPE_MARKER:
        return "marker";
    default:
        return "other";
    }
}

// The OpenGL 4.5+ device. Direct State Access only (glCreate*, glNamed*,
// glTexture*), so no object is ever bound just to edit it, and immutable
// storage only, so the driver knows every size up front (Section 7.5).
// Command lists are replayed by gl::CommandReplayer (gl_commands.cpp).
//
// Thread safety: main thread only. The debug message counters are atomic
// because asynchronous debug output may call back on a driver thread.
class GlDevice final : public Device {
public:
    GlDevice(platform::Window& window, const GlDeviceOptions& options)
        : m_window(window)
        , m_options(options)
        , m_replayer(m_resources, m_state, m_capabilities, window) {}

    GlDevice(const GlDevice&) = delete;
    GlDevice& operator=(const GlDevice&) = delete;
    GlDevice(GlDevice&&) = delete;
    GlDevice& operator=(GlDevice&&) = delete;

    // Logging leaks allocates; failing at shutdown cannot be recovered from,
    // so terminating (what an escaping exception does here) is acceptable.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~GlDevice() override {
        waitIdle();
        for (const std::string& leak : liveResources()) {
            VXE_LOG_WARN(log::rhi, "OpenGL device destroyed with a live {}", leak);
        }
        m_replayer.shutdown();
        if (m_readback.id != 0) {
            glDeleteBuffers(1, &m_readback.id);
        }
        freeLeftovers();
        if (m_capabilities.debugOutput) {
            // The callback points at this object, so it must go first.
            glDebugMessageCallback(nullptr, nullptr);
            glDisable(GL_DEBUG_OUTPUT);
        }
    }

    // Detects features and sets the global state every later call relies on.
    Result<void> initialize() {
        GLint major = 0;
        GLint minor = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &major);
        glGetIntegerv(GL_MINOR_VERSION, &minor);
        if (major < 4 || (major == 4 && minor < 5)) {
            return Error(ErrorCode::Unsupported,
                         std::format("OpenGL 4.5 or newer is required, the driver offers {}.{} ({})", major,
                                     minor, glString(GL_RENDERER)));
        }
        detectCapabilities(major, minor);
        if (m_options.debugOutput) {
            enableDebugOutput();
        }
        // Uploads and readbacks are tightly packed (TextureUpload), so rows
        // must not be padded to 4 bytes, which is OpenGL's default.
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        // Reversed Z (ADR-010): depth goes from 0 to 1 instead of -1 to 1, so
        // the float depth buffer keeps its precision all the way to the horizon.
        glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
        m_replayer.initialize();
        logCapabilities();
        return {};
    }

    [[nodiscard]] const DeviceCapabilities& capabilities() const noexcept override { return m_capabilities; }

    [[nodiscard]] DeviceStats stats() const override {
        DeviceStats stats;
        m_resources.buffers.forEach(
            [&stats](BufferHandle, const gl::Buffer& buffer) { stats.bufferBytes += buffer.size; });
        m_resources.textures.forEach(
            [&stats](TextureHandle, const gl::Texture& texture) { stats.textureBytes += texture.bytes; });
        stats.buffers = static_cast<u32>(m_resources.buffers.liveCount());
        stats.textures = static_cast<u32>(m_resources.textures.liveCount());
        stats.samplers = static_cast<u32>(m_resources.samplers.liveCount());
        stats.shaders = static_cast<u32>(m_resources.shaders.liveCount());
        stats.pipelines = static_cast<u32>(m_resources.pipelines.liveCount());
        const gl::ReplayStats& replay = m_replayer.stats();
        stats.commandsSubmitted = replay.commands;
        stats.invalidCommands = replay.invalidCommands;
        stats.pipelineBinds = replay.pipelineBinds;
        stats.pipelineBindsSkipped = replay.pipelineBindsSkipped;
        stats.drawCalls = replay.drawCalls;
        stats.dispatches = replay.dispatches;
        stats.stateChanges = m_state.stateChanges();
        stats.framesPresented = m_framesPresented;
        stats.pendingDestroys = static_cast<u32>(m_deferred.size());
        stats.apiErrors = m_apiErrors.load(std::memory_order_relaxed);
        stats.apiWarnings = m_apiWarnings.load(std::memory_order_relaxed);
        return stats;
    }

    Result<BufferHandle> createBuffer(const BufferDesc& desc) override {
        VXE_TRY(validate(desc));
        clearErrors();
        GLuint id = 0;
        glCreateBuffers(1, &id);
        // Dynamic storage permits updateBuffer (glNamedBufferSubData). Staging
        // buffers are mapped once, persistently and coherently, so CPU writes
        // become visible to the GPU without explicit flushes (Section 7.5).
        const bool cpuWrite = hasFlag(desc.usage, BufferUsage::CpuWrite);
        constexpr GLbitfield kMapFlags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        const GLbitfield storageFlags = GL_DYNAMIC_STORAGE_BIT | (cpuWrite ? kMapFlags : 0U);
        glNamedBufferStorage(id, static_cast<GLsizeiptr>(desc.sizeBytes), nullptr, storageFlags);
        std::byte* mapped = nullptr;
        if (cpuWrite) {
            mapped = static_cast<std::byte*>(
                glMapNamedBufferRange(id, 0, static_cast<GLsizeiptr>(desc.sizeBytes), kMapFlags));
        }
        if (const GLenum error = glGetError(); error != GL_NO_ERROR || (cpuWrite && mapped == nullptr)) {
            glDeleteBuffers(1, &id);
            return creationError("buffer", desc.debugName, error);
        }
        std::string name = objectName(desc.debugName, "buffer", m_unnamed);
        label(GL_BUFFER, id, name);
        return m_resources.buffers.allocate(gl::Buffer{.id = id,
                                                       .size = desc.sizeBytes,
                                                       .usage = desc.usage,
                                                       .mapped = mapped,
                                                       .name = std::move(name)});
    }

    Result<TextureHandle> createTexture(const TextureDesc& desc) override {
        VXE_TRY(validate(desc));
        if (desc.width > m_capabilities.maxTextureSize || desc.height > m_capabilities.maxTextureSize ||
            desc.layers > m_capabilities.maxTextureArrayLayers) {
            return Error(ErrorCode::Unsupported,
                         std::format("Texture '{}' ({}x{}, {} layers) exceeds the device limits",
                                     desc.debugName, desc.width, desc.height, desc.layers));
        }
        clearErrors();
        const gl::FormatInfo format = gl::formatInfo(desc.format);
        GLuint id = 0;
        glCreateTextures(gl::textureTarget(desc.type), 1, &id);
        if (desc.type == TextureType::Texture2DArray) {
            glTextureStorage3D(id, static_cast<GLsizei>(desc.mipLevels), format.internalFormat,
                               static_cast<GLsizei>(desc.width), static_cast<GLsizei>(desc.height),
                               static_cast<GLsizei>(desc.layers));
        } else {
            glTextureStorage2D(id, static_cast<GLsizei>(desc.mipLevels), format.internalFormat,
                               static_cast<GLsizei>(desc.width), static_cast<GLsizei>(desc.height));
        }
        if (const GLenum error = glGetError(); error != GL_NO_ERROR) {
            glDeleteTextures(1, &id);
            return creationError("texture", desc.debugName, error);
        }
        std::string name = objectName(desc.debugName, "texture", m_unnamed);
        label(GL_TEXTURE, id, name);
        TextureDesc stored = desc;
        stored.debugName = {};
        return m_resources.textures.allocate(
            gl::Texture{.id = id, .desc = stored, .name = std::move(name), .bytes = textureTotalBytes(desc)});
    }

    Result<SamplerHandle> createSampler(const SamplerDesc& desc) override {
        VXE_TRY(validate(desc));
        GLuint id = 0;
        glCreateSamplers(1, &id);
        glSamplerParameteri(id, GL_TEXTURE_MIN_FILTER,
                            static_cast<GLint>(gl::minFilter(desc.minFilter, desc.mipmapMode)));
        glSamplerParameteri(id, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(gl::magFilter(desc.magFilter)));
        glSamplerParameteri(id, GL_TEXTURE_WRAP_S, static_cast<GLint>(gl::addressMode(desc.addressU)));
        glSamplerParameteri(id, GL_TEXTURE_WRAP_T, static_cast<GLint>(gl::addressMode(desc.addressV)));
        if (desc.maxAnisotropy > 1.0F && m_capabilities.maxAnisotropy > 1.0F) {
            // Clamped, so a high quality preset never asks more than the GPU can do.
            glSamplerParameterf(id, GL_TEXTURE_MAX_ANISOTROPY,
                                std::min(desc.maxAnisotropy, m_capabilities.maxAnisotropy));
        }
        std::string name = objectName(desc.debugName, "sampler", m_unnamed);
        label(GL_SAMPLER, id, name);
        return m_resources.samplers.allocate(gl::Sampler{.id = id, .name = std::move(name)});
    }

    Result<ShaderHandle> createShader(const ShaderDesc& desc) override {
        VXE_TRY(validate(desc));
        const GLuint id = glCreateShader(gl::shaderStage(desc.stage));
        const GLchar* text = desc.source.data();
        const auto length = static_cast<GLint>(desc.source.size());
        glShaderSource(id, 1, &text, &length);
        glCompileShader(id);
        GLint compiled = GL_FALSE;
        glGetShaderiv(id, GL_COMPILE_STATUS, &compiled);
        if (compiled == GL_FALSE) {
            const std::string log = infoLog(id, glGetShaderiv, glGetShaderInfoLog);
            glDeleteShader(id);
            return Error(ErrorCode::ParseError,
                         std::format("The {} shader '{}' does not compile:\n{}", nameOf(desc.stage),
                                     desc.debugName, annotateShaderLog(log, desc.fileNames)));
        }
        std::string name = objectName(desc.debugName, "shader", m_unnamed);
        label(GL_SHADER, id, name);
        return m_resources.shaders.allocate(
            gl::Shader{.id = id, .stage = desc.stage, .name = std::move(name)});
    }

    Result<PipelineHandle> createPipeline(const PipelineDesc& desc) override {
        VXE_TRY(validate(desc));
        std::array<GLuint, 2> stages{};
        usize stageCount = 0;
        const auto addStage = [&](ShaderHandle handle, ShaderStage stage) -> Result<void> {
            if (!handle.isValid()) {
                return {};
            }
            const gl::Shader* shader = m_resources.shaders.get(handle);
            if (shader == nullptr || shader->stage != stage) {
                return Error(ErrorCode::InvalidArgument,
                             std::format("Pipeline '{}' uses a missing {} shader or one of another stage",
                                         desc.debugName, nameOf(stage)));
            }
            stages[stageCount++] = shader->id;
            return {};
        };
        VXE_TRY(addStage(desc.compute, ShaderStage::Compute));
        VXE_TRY(addStage(desc.vertex, ShaderStage::Vertex));
        VXE_TRY(addStage(desc.fragment, ShaderStage::Fragment));

        const GLuint program = glCreateProgram();
        for (usize i = 0; i < stageCount; ++i) {
            glAttachShader(program, stages[i]);
        }
        glLinkProgram(program);
        // Detached after linking, so the shaders can be destroyed while the
        // pipeline lives on; the program keeps its own linked copy.
        for (usize i = 0; i < stageCount; ++i) {
            glDetachShader(program, stages[i]);
        }
        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked == GL_FALSE) {
            const std::string log = infoLog(program, glGetProgramiv, glGetProgramInfoLog);
            glDeleteProgram(program);
            return Error(ErrorCode::ParseError,
                         std::format("Pipeline '{}' does not link:\n{}", desc.debugName, log));
        }
        std::string name = objectName(desc.debugName, "pipeline", m_unnamed);
        label(GL_PROGRAM, program, name);
        return m_resources.pipelines.allocate(gl::Pipeline{.program = program,
                                                           .compute = desc.compute.isValid(),
                                                           .topology = gl::primitiveTopology(desc.topology),
                                                           .raster = desc.raster,
                                                           .depth = desc.depth,
                                                           .blend = desc.blend,
                                                           .name = std::move(name)});
    }

    // Destroying invalidates the handle at once; the GL object itself is
    // deleted once the GPU has finished the frames that may use it (E6.6).
    void destroy(BufferHandle handle) override {
        if (auto buffer = m_resources.buffers.release(handle)) {
            deferDelete(gl::DeferredObject::Kind::Buffer, buffer->id);
        }
    }
    void destroy(TextureHandle handle) override {
        if (auto texture = m_resources.textures.release(handle)) {
            m_replayer.onTextureDestroyed(texture->id);
            deferDelete(gl::DeferredObject::Kind::Texture, texture->id);
        }
    }
    void destroy(SamplerHandle handle) override {
        if (auto sampler = m_resources.samplers.release(handle)) {
            deferDelete(gl::DeferredObject::Kind::Sampler, sampler->id);
        }
    }
    void destroy(ShaderHandle handle) override {
        if (auto shader = m_resources.shaders.release(handle)) {
            deferDelete(gl::DeferredObject::Kind::Shader, shader->id);
        }
    }
    void destroy(PipelineHandle handle) override {
        if (auto pipeline = m_resources.pipelines.release(handle)) {
            // The caches forget the program now: once deleted, OpenGL may hand
            // its name to a new program, which must not look already bound.
            m_state.forgetProgram(pipeline->program);
            m_replayer.onPipelineDestroyed(handle);
            deferDelete(gl::DeferredObject::Kind::Program, pipeline->program);
        }
    }

    Result<void> updateBuffer(BufferHandle handle, usize offset, std::span<const std::byte> data) override {
        const gl::Buffer* buffer = m_resources.buffers.get(handle);
        if (buffer == nullptr) {
            return Error(ErrorCode::InvalidArgument, "updateBuffer on a destroyed or invalid buffer");
        }
        VXE_TRY(validateBufferWrite(buffer->size, buffer->usage, buffer->name, offset, data.size()));
        glNamedBufferSubData(buffer->id, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(data.size()),
                             data.data());
        return {};
    }

    Result<void> readBuffer(BufferHandle handle, usize offset, std::span<std::byte> out) override {
        const gl::Buffer* buffer = m_resources.buffers.get(handle);
        if (buffer == nullptr) {
            return Error(ErrorCode::InvalidArgument, "readBuffer on a destroyed or invalid buffer");
        }
        VXE_TRY(validateBufferRead(buffer->size, buffer->usage, buffer->name, offset, out.size()));
        // Reading a GPU buffer directly (glGetNamedBufferSubData) makes some
        // drivers move the whole buffer to system memory for good, which
        // slows down every later GPU use. So the range is copied into a host
        // side readback buffer instead, the way a Vulkan backend reads back,
        // and the CPU waits for that copy only. Still slow: it waits for the GPU.
        VXE_TRY(ensureReadbackCapacity(out.size()));
        glCopyNamedBufferSubData(buffer->id, m_readback.id, static_cast<GLintptr>(offset), 0,
                                 static_cast<GLsizeiptr>(out.size()));
        glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
        VXE_TRY(waitForGpu());
        std::copy_n(m_readback.mapped, out.size(), out.begin());
        return {};
    }

    Result<void> readTexture(TextureHandle handle, u32 mipLevel, u32 layer,
                             std::span<std::byte> out) override {
        const gl::Texture* texture = m_resources.textures.get(handle);
        if (texture == nullptr) {
            return Error(ErrorCode::InvalidArgument, "readTexture on a destroyed or invalid texture");
        }
        VXE_TRY(validateTextureRead(texture->desc, texture->name, mipLevel, layer, out.size()));
        const gl::FormatInfo format = gl::formatInfo(texture->desc.format);
        const auto width = static_cast<GLsizei>(std::max(texture->desc.width >> mipLevel, 1U));
        const auto height = static_cast<GLsizei>(std::max(texture->desc.height >> mipLevel, 1U));
        // Rows come back bottom row first, OpenGL's lower left origin (ADR-016).
        glGetTextureSubImage(texture->id, static_cast<GLint>(mipLevel), 0, 0, static_cast<GLint>(layer),
                             width, height, 1, format.format, format.type, static_cast<GLsizei>(out.size()),
                             out.data());
        return {};
    }

    Result<void> uploadTexture(TextureHandle handle, const TextureUpload& upload) override {
        const gl::Texture* texture = m_resources.textures.get(handle);
        if (texture == nullptr) {
            return Error(ErrorCode::InvalidArgument, "uploadTexture on a destroyed or invalid texture");
        }
        VXE_TRY(validateTextureUpload(texture->desc, texture->name, upload));
        const gl::FormatInfo format = gl::formatInfo(texture->desc.format);
        const auto level = static_cast<GLint>(upload.mipLevel);
        const auto width = static_cast<GLsizei>(std::max(texture->desc.width >> upload.mipLevel, 1U));
        const auto height = static_cast<GLsizei>(std::max(texture->desc.height >> upload.mipLevel, 1U));
        if (texture->desc.type == TextureType::Texture2DArray) {
            glTextureSubImage3D(texture->id, level, 0, 0, static_cast<GLint>(upload.layer), width, height, 1,
                                format.format, format.type, upload.data.data());
        } else {
            glTextureSubImage2D(texture->id, level, 0, 0, width, height, format.format, format.type,
                                upload.data.data());
        }
        return {};
    }

    std::span<std::byte> mappedRange(BufferHandle handle) override {
        const gl::Buffer* buffer = m_resources.buffers.get(handle);
        if (buffer == nullptr || buffer->mapped == nullptr) {
            return {};
        }
        return {buffer->mapped, buffer->size};
    }

    FenceHandle insertFence() override {
        return m_resources.fences.allocate(gl::Fence{.sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0)});
    }

    bool isFenceSignaled(FenceHandle fence) override {
        const gl::Fence* entry = m_resources.fences.get(fence);
        if (entry == nullptr) {
            return false;
        }
        // A zero timeout only polls. The flush bit makes sure the fence was
        // actually sent to the GPU, otherwise polling could wait forever.
        const GLenum status = glClientWaitSync(entry->sync, GL_SYNC_FLUSH_COMMANDS_BIT, 0);
        return status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED;
    }

    void destroy(FenceHandle fence) override {
        if (auto entry = m_resources.fences.release(fence)) {
            glDeleteSync(entry->sync);
        }
    }

    Result<QueryHandle> createTimestampQuery() override {
        if (!m_capabilities.timestampQueries) {
            return Error(ErrorCode::Unsupported, "This driver has no timestamp queries");
        }
        GLuint id = 0;
        glCreateQueries(GL_TIMESTAMP, 1, &id);
        return m_resources.queries.allocate(gl::Query{.id = id});
    }

    void destroy(QueryHandle query) override {
        if (auto entry = m_resources.queries.release(query)) {
            glDeleteQueries(1, &entry->id);
        }
    }

    std::optional<u64> timestampNanoseconds(QueryHandle query) override {
        const gl::Query* entry = m_resources.queries.get(query);
        if (entry == nullptr || !entry->written) {
            return std::nullopt;
        }
        GLint available = GL_FALSE;
        glGetQueryObjectiv(entry->id, GL_QUERY_RESULT_AVAILABLE, &available);
        if (available == GL_FALSE) {
            return std::nullopt;
        }
        GLuint64 nanoseconds = 0;
        glGetQueryObjectui64v(entry->id, GL_QUERY_RESULT, &nanoseconds);
        return static_cast<u64>(nanoseconds);
    }

    void submit(const CommandList& commands) override { m_replayer.submit(commands); }

    void beginFrame() override { releaseFinishedFrames(); }

    void present() override {
        m_window.swapGlBuffers();
        ++m_framesPresented;
        // Marks the end of this frame in the GPU's command stream. When the
        // fence signals, everything destroyed during the frame can go.
        m_frameFences.push_back(
            {.serial = m_frameSerial, .sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0)});
        ++m_frameSerial;
        releaseFinishedFrames();
    }

    void waitIdle() override {
        glFinish();
        for (const FrameFence& fence : m_frameFences) {
            glDeleteSync(fence.sync);
        }
        m_frameFences.clear();
        m_completedSerial = m_frameSerial;
        m_deferred.releaseAll([](const gl::DeferredObject& object) { deleteObject(object); });
    }

    [[nodiscard]] std::vector<std::string> liveResources() const override {
        std::vector<std::string> live;
        m_resources.buffers.forEach(
            [&live](BufferHandle, const gl::Buffer& b) { live.push_back("buffer '" + b.name + "'"); });
        m_resources.textures.forEach(
            [&live](TextureHandle, const gl::Texture& t) { live.push_back("texture '" + t.name + "'"); });
        m_resources.samplers.forEach(
            [&live](SamplerHandle, const gl::Sampler& s) { live.push_back("sampler '" + s.name + "'"); });
        m_resources.shaders.forEach(
            [&live](ShaderHandle, const gl::Shader& s) { live.push_back("shader '" + s.name + "'"); });
        m_resources.pipelines.forEach(
            [&live](PipelineHandle, const gl::Pipeline& p) { live.push_back("pipeline '" + p.name + "'"); });
        m_resources.fences.forEach([&live](FenceHandle, const gl::Fence&) { live.emplace_back("fence"); });
        m_resources.queries.forEach(
            [&live](QueryHandle, const gl::Query&) { live.emplace_back("timestamp query"); });
        return live;
    }

    void setPresentMode(PresentMode mode) override {
        switch (mode) {
        case PresentMode::Immediate:
        case PresentMode::Mailbox: // Emulated by the frame pacer (E8.4), so the driver runs uncapped.
            static_cast<void>(m_window.setGlSwapInterval(0));
            break;
        case PresentMode::Vsync:
            static_cast<void>(m_window.setGlSwapInterval(1));
            break;
        case PresentMode::Adaptive:
            // Not every driver supports adaptive vsync (-1); plain vsync is the closest fallback.
            if (!m_window.setGlSwapInterval(-1)) {
                static_cast<void>(m_window.setGlSwapInterval(1));
            }
            break;
        }
    }

    [[nodiscard]] u32 backBufferWidth() const noexcept override {
        return static_cast<u32>(std::max(m_window.pixelSize().x, 0));
    }
    [[nodiscard]] u32 backBufferHeight() const noexcept override {
        return static_cast<u32>(std::max(m_window.pixelSize().y, 0));
    }

private:
    void detectCapabilities(GLint major, GLint minor) {
        DeviceCapabilities& caps = m_capabilities;
        caps.backend = Backend::OpenGL;
        caps.apiVersionMajor = major;
        caps.apiVersionMinor = minor;
        caps.rendererName = glString(GL_RENDERER);
        caps.vendorName = glString(GL_VENDOR);
        caps.driverVersion = glString(GL_VERSION);

        const bool gl46 = GLAD_GL_VERSION_4_6 != 0;
        caps.multiDrawIndirectCount = gl46 || GLAD_GL_ARB_indirect_parameters != 0;
        caps.spirvShaders = gl46 || GLAD_GL_ARB_gl_spirv != 0;
        caps.bindlessTextures = GLAD_GL_ARB_bindless_texture != 0;

        GLint timestampBits = 0;
        glGetQueryiv(GL_TIMESTAMP, GL_QUERY_COUNTER_BITS, &timestampBits);
        caps.timestampQueries = timestampBits > 0;

        caps.maxTextureArrayLayers = glLimit(GL_MAX_ARRAY_TEXTURE_LAYERS);
        caps.maxTextureSize = glLimit(GL_MAX_TEXTURE_SIZE);
        caps.maxComputeWorkGroupInvocations = glLimit(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS);
        caps.uniformBufferOffsetAlignment = std::max(glLimit(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT), 1U);
        caps.storageBufferOffsetAlignment = std::max(glLimit(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT), 1U);

        // Core in 4.6; the ARB and EXT extensions use the same enum value.
        if (gl46 || GLAD_GL_ARB_texture_filter_anisotropic != 0 ||
            GLAD_GL_EXT_texture_filter_anisotropic != 0) {
            GLfloat anisotropy = 1.0F;
            glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &anisotropy);
            caps.maxAnisotropy = anisotropy;
        }
        // Only NVIDIA reports total dedicated memory. AMD's ATI_meminfo
        // reports free memory instead, which the overlay can show later.
        if (GLAD_GL_NVX_gpu_memory_info != 0) {
            GLint kilobytes = 0;
            glGetIntegerv(GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX, &kilobytes);
            caps.videoMemoryBytes = static_cast<u64>(std::max(kilobytes, 0)) * 1024U;
        }
    }

    void enableDebugOutput() {
        glEnable(GL_DEBUG_OUTPUT);
        if (m_options.synchronousDebugOutput) {
            glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        }
        glDebugMessageCallback(&GlDevice::debugCallback, this);
        // Drivers differ in which severities start enabled (low is often off),
        // so enable everything; notifications are switched off again below.
        glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, GL_TRUE);
        // Prove the callback works by sending one marker through it, so
        // "debug output: yes" and a zero error count can be trusted. Only
        // possible with synchronous output, where the marker arrives before
        // glDebugMessageInsert returns.
        if (m_options.synchronousDebugOutput) {
            glDebugMessageInsert(GL_DEBUG_SOURCE_APPLICATION, GL_DEBUG_TYPE_MARKER, kSelfTestMessageId,
                                 GL_DEBUG_SEVERITY_NOTIFICATION, -1, "vxe debug output self test");
            m_capabilities.debugOutput = m_debugSelfTestSeen.load(std::memory_order_relaxed);
            if (!m_capabilities.debugOutput) {
                VXE_LOG_WARN(log::rhi,
                             "Debug output is enabled, but the driver did not deliver a test message");
            }
        } else {
            m_capabilities.debugOutput = true;
        }
        // Notifications (for example NVIDIA's "buffer will use video memory")
        // would flood the log with every resource creation.
        glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_NOTIFICATION, 0, nullptr,
                              GL_FALSE);
    }

    static void GLAD_API_PTR debugCallback(GLenum source, GLenum type, GLuint id, GLenum severity,
                                           GLsizei length, const GLchar* message, const void* userParam) {
        const auto* device = static_cast<const GlDevice*>(userParam);
        const std::string_view text(message, length < 0 ? std::char_traits<char>::length(message)
                                                        : static_cast<usize>(length));
        if (source == GL_DEBUG_SOURCE_APPLICATION && id == kSelfTestMessageId) {
            device->m_debugSelfTestSeen.store(true, std::memory_order_relaxed);
            return;
        }
        // Compile and link failures are returned as errors with file names
        // (createShader, createPipeline), so they need no second report here.
        if (type == GL_DEBUG_TYPE_PUSH_GROUP || type == GL_DEBUG_TYPE_POP_GROUP ||
            source == GL_DEBUG_SOURCE_SHADER_COMPILER) {
            return;
        }
        if (type == GL_DEBUG_TYPE_ERROR || severity == GL_DEBUG_SEVERITY_HIGH) {
            device->m_apiErrors.fetch_add(1, std::memory_order_relaxed);
            VXE_LOG_ERROR(log::rhi, "GL {} ({}, id {}): {}", debugTypeName(type), debugSourceName(source), id,
                          text);
        } else if (severity == GL_DEBUG_SEVERITY_MEDIUM || severity == GL_DEBUG_SEVERITY_LOW) {
            device->m_apiWarnings.fetch_add(1, std::memory_order_relaxed);
            VXE_LOG_WARN(log::rhi, "GL {} ({}, id {}): {}", debugTypeName(type), debugSourceName(source), id,
                         text);
        } else {
            VXE_LOG_TRACE(log::rhi, "GL {} ({}, id {}): {}", debugTypeName(type), debugSourceName(source), id,
                          text);
        }
    }

    void logCapabilities() const {
        constexpr u64 kBytesPerMiB = u64{1024} * 1024;
        const DeviceCapabilities& caps = m_capabilities;
        const auto yesNo = [](bool value) { return value ? "yes" : "no"; };
        VXE_LOG_INFO(log::rhi, "OpenGL {}.{} on {} ({}), driver {}", caps.apiVersionMajor,
                     caps.apiVersionMinor, caps.rendererName, caps.vendorName, caps.driverVersion);
        VXE_LOG_INFO(
            log::rhi,
            "Indirect count draws: {}, bindless textures: {}, SPIR-V: {}, timestamps: {}, debug output: {}",
            yesNo(caps.multiDrawIndirectCount), yesNo(caps.bindlessTextures), yesNo(caps.spirvShaders),
            yesNo(caps.timestampQueries), yesNo(caps.debugOutput));
        VXE_LOG_INFO(
            log::rhi,
            "Max texture {} px, {} array layers, {} compute invocations, anisotropy {}x, video memory {} MiB",
            caps.maxTextureSize, caps.maxTextureArrayLayers, caps.maxComputeWorkGroupInvocations,
            caps.maxAnisotropy, caps.videoMemoryBytes / kBytesPerMiB);
    }

    void deferDelete(gl::DeferredObject::Kind kind, GLuint id) {
        m_deferred.push(gl::DeferredObject{.kind = kind, .id = id}, m_frameSerial);
    }

    static void deleteObject(const gl::DeferredObject& object) {
        switch (object.kind) {
        case gl::DeferredObject::Kind::Buffer:
            glDeleteBuffers(1, &object.id); // Also ends a persistent mapping.
            break;
        case gl::DeferredObject::Kind::Texture:
            glDeleteTextures(1, &object.id);
            break;
        case gl::DeferredObject::Kind::Sampler:
            glDeleteSamplers(1, &object.id);
            break;
        case gl::DeferredObject::Kind::Shader:
            glDeleteShader(object.id);
            break;
        case gl::DeferredObject::Kind::Program:
            glDeleteProgram(object.id);
            break;
        }
    }

    // Checks the frame fences without waiting, oldest first, and deletes the
    // objects of every frame the GPU has completed.
    void releaseFinishedFrames() {
        while (!m_frameFences.empty()) {
            const GLenum status = glClientWaitSync(m_frameFences.front().sync, 0, 0);
            if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) {
                break;
            }
            m_completedSerial = m_frameFences.front().serial;
            glDeleteSync(m_frameFences.front().sync);
            m_frameFences.pop_front();
        }
        m_deferred.releaseCompleted(m_completedSerial,
                                    [](const gl::DeferredObject& object) { deleteObject(object); });
    }

    // Grows the readback buffer to at least `size` bytes. It lives in system
    // memory (client storage) and stays mapped, so reads need no extra copy.
    Result<void> ensureReadbackCapacity(usize size) {
        if (m_readback.size >= size) {
            return {};
        }
        if (m_readback.id != 0) {
            glDeleteBuffers(1, &m_readback.id);
            m_readback = {};
        }
        const usize capacity = std::max<usize>(std::bit_ceil(size), usize{64} * 1024);
        constexpr GLbitfield kMapFlags = GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        clearErrors();
        glCreateBuffers(1, &m_readback.id);
        glNamedBufferStorage(m_readback.id, static_cast<GLsizeiptr>(capacity), nullptr,
                             kMapFlags | GL_CLIENT_STORAGE_BIT);
        m_readback.mapped = static_cast<std::byte*>(
            glMapNamedBufferRange(m_readback.id, 0, static_cast<GLsizeiptr>(capacity), kMapFlags));
        if (const GLenum error = glGetError(); error != GL_NO_ERROR || m_readback.mapped == nullptr) {
            glDeleteBuffers(1, &m_readback.id);
            m_readback = {};
            return creationError("buffer", "Readback", error);
        }
        label(GL_BUFFER, m_readback.id, "Readback");
        m_readback.size = capacity;
        return {};
    }

    // Blocks until the GPU has executed every command sent so far.
    static Result<void> waitForGpu() {
        auto* const fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        constexpr GLuint64 kTimeoutNanoseconds = 10'000'000'000; // Far beyond any sane frame.
        const GLenum status = glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, kTimeoutNanoseconds);
        glDeleteSync(fence);
        if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) {
            return Error(ErrorCode::Timeout,
                         "The GPU did not finish within 10 seconds (device lost or hung)");
        }
        return {};
    }

    // Errors from earlier calls must not be blamed on the next creation.
    static void clearErrors() {
        while (glGetError() != GL_NO_ERROR) {
        }
    }

    static Error creationError(std::string_view what, std::string_view name, GLenum error) {
        if (error == GL_OUT_OF_MEMORY) {
            return Error(ErrorCode::OutOfMemory,
                         std::format("Out of video memory creating {} '{}'", what, name));
        }
        return Error(ErrorCode::Unknown, std::format("OpenGL error 0x{:04X} creating {} '{}'",
                                                     static_cast<u32>(error), what, name));
    }

    // Frees what the caller forgot to destroy. The destructor has already
    // logged them as leaks through liveResources().
    void freeLeftovers() const {
        m_resources.buffers.forEach([](BufferHandle, const gl::Buffer& b) { glDeleteBuffers(1, &b.id); });
        m_resources.textures.forEach([](TextureHandle, const gl::Texture& t) { glDeleteTextures(1, &t.id); });
        m_resources.samplers.forEach(
            [](SamplerHandle, const gl::Sampler& sm) { glDeleteSamplers(1, &sm.id); });
        m_resources.shaders.forEach([](ShaderHandle, const gl::Shader& sh) { glDeleteShader(sh.id); });
        m_resources.pipelines.forEach(
            [](PipelineHandle, const gl::Pipeline& pl) { glDeleteProgram(pl.program); });
        m_resources.fences.forEach([](FenceHandle, const gl::Fence& f) { glDeleteSync(f.sync); });
        m_resources.queries.forEach([](QueryHandle, const gl::Query& q) { glDeleteQueries(1, &q.id); });
    }

    platform::Window& m_window;
    GlDeviceOptions m_options;
    DeviceCapabilities m_capabilities;
    gl::Resources m_resources;
    gl::StateCache m_state;
    gl::CommandReplayer m_replayer;
    // Host side buffer for readBuffer, grown on demand. Not a handle: it
    // belongs to the device, not to any caller.
    struct Readback {
        GLuint id = 0;
        usize size = 0;
        std::byte* mapped = nullptr;
    } m_readback;
    u64 m_framesPresented = 0;
    u32 m_unnamed = 0; // Counter for "unnamed <kind> <n>" names.

    // Frame tracking for deferred destruction: the serial of the frame being
    // recorded, the newest frame the GPU has completed, and one fence per
    // presented frame that has not signaled yet.
    struct FrameFence {
        u64 serial = 0;
        GLsync sync = nullptr;
    };
    u64 m_frameSerial = 1;
    u64 m_completedSerial = 0;
    std::deque<FrameFence> m_frameFences;
    DeferredReleaseQueue<gl::DeferredObject> m_deferred;

    mutable std::atomic<u64> m_apiErrors{0};
    mutable std::atomic<u64> m_apiWarnings{0};
    mutable std::atomic<bool> m_debugSelfTestSeen{false};

    // Any id works; this one spells "VXE1" in ASCII so it is easy to spot.
    static constexpr GLuint kSelfTestMessageId = 0x56584531;
};

} // namespace

Result<std::unique_ptr<Device>> createGlDevice(platform::Window& window, const GlDeviceOptions& options) {
    if (window.glContext() == nullptr) {
        return Error(ErrorCode::InvalidArgument,
                     "The OpenGL device needs a window created with GraphicsApi::OpenGL");
    }
    // glad keeps the loaded functions in globals, so loading again for a
    // second device is harmless; the engine only ever creates one.
    if (gladLoadGL(&platform::Window::glGetProcAddress) == 0) {
        return Error(ErrorCode::Unsupported, "Could not load the OpenGL functions");
    }
    auto device = std::make_unique<GlDevice>(window, options);
    VXE_TRY(device->initialize());
    return std::unique_ptr<Device>(std::move(device));
}

} // namespace vxe::rhi
