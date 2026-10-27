#include "vxe/rhi_gl/gl_device.hpp"

#include "gl_formats.hpp"
#include "gl_state.hpp"

#include <vxe/core/log.hpp>
#include <vxe/platform/window.hpp>
#include <vxe/rhi/shader_source.hpp>
#include <vxe/rhi/validation.hpp>

#include <glad/gl.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace vxe::rhi {
namespace {

struct GlBuffer {
    GLuint id = 0;
    usize size = 0;
    BufferUsage usage = BufferUsage::None;
    std::byte* mapped = nullptr; // Persistent mapping of CpuWrite buffers, valid until deletion.
    std::string name;
};

struct GlTexture {
    GLuint id = 0;
    TextureDesc desc; // debugName cleared; `name` owns the text.
    std::string name;
    usize bytes = 0;
};

struct GlSampler {
    GLuint id = 0;
};

struct GlFence {
    GLsync sync = nullptr;
};

struct GlShader {
    GLuint id = 0;
    ShaderStage stage = ShaderStage::Vertex;
    std::string name;
};

// A linked program plus every piece of fixed function state, applied as a
// whole on bind (through the StateCache, which skips what is already set).
struct GlPipeline {
    GLuint program = 0;
    bool compute = false;
    GLenum topology = GL_TRIANGLES; // Used by draws (E6.5).
    RasterState raster;
    DepthState depth;
    std::array<BlendState, kMaxColorAttachments> blend{};
    std::string name;
};

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
//
// Thread safety: main thread only. The debug message counters are atomic
// because asynchronous debug output may call back on a driver thread.
class GlDevice final : public Device {
public:
    GlDevice(platform::Window& window, const GlDeviceOptions& options)
        : m_window(window)
        , m_options(options) {}

    GlDevice(const GlDevice&) = delete;
    GlDevice& operator=(const GlDevice&) = delete;
    GlDevice(GlDevice&&) = delete;
    GlDevice& operator=(GlDevice&&) = delete;

    // Logging leaks allocates; failing at shutdown cannot be recovered from,
    // so terminating (what an escaping exception does here) is acceptable.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~GlDevice() override {
        reportAndFreeLeftovers();
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
        logCapabilities();
        return {};
    }

    [[nodiscard]] const DeviceCapabilities& capabilities() const noexcept override { return m_capabilities; }

    [[nodiscard]] DeviceStats stats() const override {
        DeviceStats stats;
        m_buffers.forEach(
            [&stats](BufferHandle, const GlBuffer& buffer) { stats.bufferBytes += buffer.size; });
        m_textures.forEach(
            [&stats](TextureHandle, const GlTexture& texture) { stats.textureBytes += texture.bytes; });
        stats.buffers = static_cast<u32>(m_buffers.liveCount());
        stats.textures = static_cast<u32>(m_textures.liveCount());
        stats.samplers = static_cast<u32>(m_samplers.liveCount());
        stats.shaders = static_cast<u32>(m_shaders.liveCount());
        stats.pipelines = static_cast<u32>(m_pipelines.liveCount());
        stats.commandsSubmitted = m_commandsSubmitted;
        stats.invalidCommands = m_invalidCommands;
        stats.pipelineBinds = m_pipelineBinds;
        stats.pipelineBindsSkipped = m_pipelineBindsSkipped;
        stats.stateChanges = m_state.stateChanges();
        stats.framesPresented = m_framesPresented;
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
        label(GL_BUFFER, id, desc.debugName);
        return m_buffers.allocate(GlBuffer{.id = id,
                                           .size = desc.sizeBytes,
                                           .usage = desc.usage,
                                           .mapped = mapped,
                                           .name = std::string(desc.debugName)});
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
        label(GL_TEXTURE, id, desc.debugName);
        TextureDesc stored = desc;
        stored.debugName = {};
        return m_textures.allocate(GlTexture{
            .id = id, .desc = stored, .name = std::string(desc.debugName), .bytes = textureTotalBytes(desc)});
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
        label(GL_SAMPLER, id, desc.debugName);
        return m_samplers.allocate(GlSampler{.id = id});
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
        label(GL_SHADER, id, desc.debugName);
        return m_shaders.allocate(
            GlShader{.id = id, .stage = desc.stage, .name = std::string(desc.debugName)});
    }

    Result<PipelineHandle> createPipeline(const PipelineDesc& desc) override {
        VXE_TRY(validate(desc));
        std::array<GLuint, 2> stages{};
        usize stageCount = 0;
        const auto addStage = [&](ShaderHandle handle, ShaderStage stage) -> Result<void> {
            if (!handle.isValid()) {
                return {};
            }
            const GlShader* shader = m_shaders.get(handle);
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
        label(GL_PROGRAM, program, desc.debugName);
        return m_pipelines.allocate(GlPipeline{.program = program,
                                               .compute = desc.compute.isValid(),
                                               .topology = gl::primitiveTopology(desc.topology),
                                               .raster = desc.raster,
                                               .depth = desc.depth,
                                               .blend = desc.blend,
                                               .name = std::string(desc.debugName)});
    }

    void destroy(BufferHandle handle) override {
        if (auto buffer = m_buffers.release(handle)) {
            // Deleting also ends a persistent mapping. OpenGL keeps the memory
            // alive until the GPU is done with it, so this is safe right away.
            glDeleteBuffers(1, &buffer->id);
        }
    }
    void destroy(TextureHandle handle) override {
        if (auto texture = m_textures.release(handle)) {
            glDeleteTextures(1, &texture->id);
        }
    }
    void destroy(SamplerHandle handle) override {
        if (auto sampler = m_samplers.release(handle)) {
            glDeleteSamplers(1, &sampler->id);
        }
    }
    void destroy(ShaderHandle handle) override {
        if (auto shader = m_shaders.release(handle)) {
            glDeleteShader(shader->id);
        }
    }
    void destroy(PipelineHandle handle) override {
        if (auto pipeline = m_pipelines.release(handle)) {
            m_state.forgetProgram(pipeline->program);
            if (m_boundPipeline == handle) {
                m_boundPipeline = {};
            }
            glDeleteProgram(pipeline->program);
        }
    }

    Result<void> updateBuffer(BufferHandle handle, usize offset, std::span<const std::byte> data) override {
        const GlBuffer* buffer = m_buffers.get(handle);
        if (buffer == nullptr) {
            return Error(ErrorCode::InvalidArgument, "updateBuffer on a destroyed or invalid buffer");
        }
        VXE_TRY(validateBufferWrite(buffer->size, buffer->usage, buffer->name, offset, data.size()));
        glNamedBufferSubData(buffer->id, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(data.size()),
                             data.data());
        return {};
    }

    Result<void> readBuffer(BufferHandle handle, usize offset, std::span<std::byte> out) override {
        const GlBuffer* buffer = m_buffers.get(handle);
        if (buffer == nullptr) {
            return Error(ErrorCode::InvalidArgument, "readBuffer on a destroyed or invalid buffer");
        }
        VXE_TRY(validateBufferRead(buffer->size, buffer->usage, buffer->name, offset, out.size()));
        // Waits for every earlier GPU write to the buffer, which is why this is slow.
        glGetNamedBufferSubData(buffer->id, static_cast<GLintptr>(offset),
                                static_cast<GLsizeiptr>(out.size()), out.data());
        return {};
    }

    Result<void> uploadTexture(TextureHandle handle, const TextureUpload& upload) override {
        const GlTexture* texture = m_textures.get(handle);
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
        const GlBuffer* buffer = m_buffers.get(handle);
        if (buffer == nullptr || buffer->mapped == nullptr) {
            return {};
        }
        return {buffer->mapped, buffer->size};
    }

    FenceHandle insertFence() override {
        return m_fences.allocate(GlFence{.sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0)});
    }

    bool isFenceSignaled(FenceHandle fence) override {
        const GlFence* entry = m_fences.get(fence);
        if (entry == nullptr) {
            return false;
        }
        // A zero timeout only polls. The flush bit makes sure the fence was
        // actually sent to the GPU, otherwise polling could wait forever.
        const GLenum status = glClientWaitSync(entry->sync, GL_SYNC_FLUSH_COMMANDS_BIT, 0);
        return status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED;
    }

    void destroy(FenceHandle fence) override {
        if (auto entry = m_fences.release(fence)) {
            glDeleteSync(entry->sync);
        }
    }

    void submit(const CommandList& commands) override {
        // E6.4 replays pipeline binds only; render passes, binds, draws and
        // dispatches follow in E6.5.
        for (const CommandView command : commands) {
            ++m_commandsSubmitted;
            if (command.type == CommandType::BindPipeline) {
                bindPipeline(command.as<cmd::BindPipeline>().pipeline);
            }
        }
    }

    void beginFrame() override {}

    void present() override {
        m_window.swapGlBuffers();
        ++m_framesPresented;
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

        GLint value = 0;
        glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &value);
        caps.maxTextureArrayLayers = static_cast<u32>(value);
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &value);
        caps.maxTextureSize = static_cast<u32>(value);
        glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &value);
        caps.maxComputeWorkGroupInvocations = static_cast<u32>(value);

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

    // Makes a pipeline current. Binding the pipeline that is already bound
    // costs nothing, and switching only sends the state that differs.
    void bindPipeline(PipelineHandle handle) {
        ++m_pipelineBinds;
        const GlPipeline* pipeline = m_pipelines.get(handle);
        if (pipeline == nullptr) {
            ++m_invalidCommands;
            return;
        }
        if (handle == m_boundPipeline) {
            ++m_pipelineBindsSkipped;
            return;
        }
        m_state.useProgram(pipeline->program);
        if (!pipeline->compute) {
            m_state.applyRaster(pipeline->raster);
            m_state.applyDepth(pipeline->depth);
            m_state.applyBlend(pipeline->blend);
        }
        m_boundPipeline = handle;
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

    // Frees what the caller forgot and names it, so leaks are easy to find.
    // The full lifetime tracking with deferred destruction follows in E6.6.
    void reportAndFreeLeftovers() {
        std::vector<std::string> leaks;
        m_buffers.forEach([&leaks](BufferHandle, const GlBuffer& b) {
            leaks.push_back("buffer '" + b.name + "'");
            glDeleteBuffers(1, &b.id);
        });
        m_textures.forEach([&leaks](TextureHandle, const GlTexture& t) {
            leaks.push_back("texture '" + t.name + "'");
            glDeleteTextures(1, &t.id);
        });
        m_samplers.forEach([](SamplerHandle, const GlSampler& s) { glDeleteSamplers(1, &s.id); });
        if (m_samplers.liveCount() > 0) {
            leaks.push_back(std::format("{} samplers", m_samplers.liveCount()));
        }
        m_shaders.forEach([&leaks](ShaderHandle, const GlShader& sh) {
            leaks.push_back("shader '" + sh.name + "'");
            glDeleteShader(sh.id);
        });
        m_pipelines.forEach([&leaks](PipelineHandle, const GlPipeline& pl) {
            leaks.push_back("pipeline '" + pl.name + "'");
            glDeleteProgram(pl.program);
        });
        m_fences.forEach([](FenceHandle, const GlFence& f) { glDeleteSync(f.sync); });
        for (const std::string& leak : leaks) {
            VXE_LOG_WARN(log::rhi, "OpenGL device destroyed with a live {}", leak);
        }
    }

    platform::Window& m_window;
    GlDeviceOptions m_options;
    DeviceCapabilities m_capabilities;
    HandlePool<BufferTag, GlBuffer> m_buffers;
    HandlePool<TextureTag, GlTexture> m_textures;
    HandlePool<SamplerTag, GlSampler> m_samplers;
    HandlePool<FenceTag, GlFence> m_fences;
    HandlePool<ShaderTag, GlShader> m_shaders;
    HandlePool<PipelineTag, GlPipeline> m_pipelines;
    gl::StateCache m_state;
    PipelineHandle m_boundPipeline;
    u64 m_commandsSubmitted = 0;
    u64 m_invalidCommands = 0;
    u64 m_pipelineBinds = 0;
    u64 m_pipelineBindsSkipped = 0;
    u64 m_framesPresented = 0;
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
