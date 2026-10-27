#pragma once

#include <vxe/core/types.hpp>
#include <vxe/rhi/handles.hpp>

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

// Plain data types of the RHI: formats, usage flags, pipeline state and the
// descriptions resources are created from (Section 7). They are shaped like
// Vulkan and D3D12 (immutable pipelines, declared usage), because those map
// well down to OpenGL while OpenGL concepts map badly up (ADR-003).
namespace vxe::rhi {

// Bit flag operators, only for enums that opt in below (so CullMode::None
// and friends cannot be combined by accident).
template <typename E>
inline constexpr bool kIsFlagEnum = false;

template <typename E>
concept FlagEnum = std::is_enum_v<E> && kIsFlagEnum<E>;

template <FlagEnum E>
[[nodiscard]] constexpr E operator|(E a, E b) noexcept {
    using U = std::underlying_type_t<E>;
    return static_cast<E>(static_cast<U>(a) | static_cast<U>(b));
}
template <FlagEnum E>
[[nodiscard]] constexpr E operator&(E a, E b) noexcept {
    using U = std::underlying_type_t<E>;
    return static_cast<E>(static_cast<U>(a) & static_cast<U>(b));
}
template <FlagEnum E>
[[nodiscard]] constexpr bool hasFlag(E flags, E flag) noexcept {
    return (flags & flag) == flag && flag != E::None;
}

enum class Backend : u8 {
    Null,
    OpenGL,
    // Vulkan (future, ADR-003)
};

enum class Format : u8 {
    Undefined,
    R8Unorm,
    RG8Unorm,
    RGBA8Unorm,
    RGBA8Srgb,
    BGRA8Unorm,
    R16Float,
    RG16Float,
    RGBA16Float,
    R32Float,
    RG32Float,
    RGBA32Float,
    R32Uint,
    RG32Uint,
    RGBA32Uint,
    R11G11B10Float,
    D32Float, // The main depth format (ADR-010).
    D24UnormS8Uint,
};

[[nodiscard]] constexpr bool isDepthFormat(Format format) noexcept {
    return format == Format::D32Float || format == Format::D24UnormS8Uint;
}

// Size of one texel in bytes, for upload size checks. 0 for Undefined.
[[nodiscard]] constexpr u32 bytesPerTexel(Format format) noexcept {
    switch (format) {
    case Format::Undefined:
        return 0;
    case Format::R8Unorm:
        return 1;
    case Format::RG8Unorm:
    case Format::R16Float:
        return 2;
    case Format::RGBA8Unorm:
    case Format::RGBA8Srgb:
    case Format::BGRA8Unorm:
    case Format::RG16Float:
    case Format::R32Float:
    case Format::R32Uint:
    case Format::R11G11B10Float:
    case Format::D32Float:
    case Format::D24UnormS8Uint:
        return 4;
    case Format::RGBA16Float:
    case Format::RG32Float:
    case Format::RG32Uint:
        return 8;
    case Format::RGBA32Float:
    case Format::RGBA32Uint:
        return 16;
    }
    return 0;
}

// What a buffer will be used for. The backend picks memory and inserts
// barriers from this, and a Vulkan backend would set the usage bits.
enum class BufferUsage : u32 {
    None = 0,
    Vertex = 1U << 0U,
    Index = 1U << 1U,
    Uniform = 1U << 2U,
    Storage = 1U << 3U,
    Indirect = 1U << 4U,
    TransferSrc = 1U << 5U,
    TransferDst = 1U << 6U,
    CpuWrite = 1U << 7U, // Persistently mapped for CPU writes (staging rings, Section 7.5).
};

enum class TextureType : u8 {
    Texture2D,
    Texture2DArray, // Block textures (Section 15.3).
};

enum class TextureUsage : u32 {
    None = 0,
    Sampled = 1U << 0U,
    RenderTarget = 1U << 1U,
    DepthTarget = 1U << 2U,
    Storage = 1U << 3U,
    TransferDst = 1U << 4U,
};

enum class Filter : u8 {
    Nearest,
    Linear
};
enum class MipmapMode : u8 {
    None,
    Nearest,
    Linear
};
enum class AddressMode : u8 {
    Repeat,
    ClampToEdge,
    MirroredRepeat
};

enum class ShaderStage : u8 {
    Vertex,
    Fragment,
    Compute
};

enum class PrimitiveTopology : u8 {
    Triangles,
    Lines,
    Points
};
enum class PolygonMode : u8 {
    Fill,
    Line
}; // Line is the wireframe debug view.
enum class CullMode : u8 {
    None,
    Front,
    Back
};
enum class FrontFace : u8 {
    CounterClockwise,
    Clockwise
};

// With reversed Z (ADR-010) nearer surfaces have larger depth, so the
// normal test is GreaterOrEqual and depth clears to 0.
enum class CompareOp : u8 {
    Never,
    Less,
    LessOrEqual,
    Equal,
    GreaterOrEqual,
    Greater,
    NotEqual,
    Always
};

enum class BlendFactor : u8 {
    Zero,
    One,
    SrcColor,
    OneMinusSrcColor,
    SrcAlpha,
    OneMinusSrcAlpha,
    DstColor,
    OneMinusDstColor,
    DstAlpha,
    OneMinusDstAlpha,
};
enum class BlendOp : u8 {
    Add,
    Subtract,
    ReverseSubtract,
    Min,
    Max
};

enum class IndexType : u8 {
    Uint16,
    Uint32
};

enum class LoadOp : u8 {
    Load,
    Clear,
    DontCare
};
enum class StoreOp : u8 {
    Store,
    DontCare
};

enum class PresentMode : u8 {
    Immediate, // Uncapped, may tear.
    Vsync,
    Adaptive, // Vsync, but tear instead of stall when a frame is late.
    Mailbox,  // Emulated on OpenGL (Section 8.5).
};

// What a resource barrier makes visible to later commands. The OpenGL
// backend turns these into glMemoryBarrier bits; Vulkan would use pipeline
// barriers (Section 7.4).
enum class BarrierScope : u32 {
    None = 0,
    VertexData = 1U << 0U,
    IndexData = 1U << 1U,
    UniformData = 1U << 2U,
    StorageData = 1U << 3U,
    IndirectCommands = 1U << 4U,
    TextureFetch = 1U << 5U,
    StorageImage = 1U << 6U,
    TransferData = 1U << 7U,
    All = 0xFFFFFFFFU,
};

template <>
inline constexpr bool kIsFlagEnum<BufferUsage> = true;
template <>
inline constexpr bool kIsFlagEnum<TextureUsage> = true;
template <>
inline constexpr bool kIsFlagEnum<BarrierScope> = true;

// ---------------------------------------------------------------------------
// Descriptions

struct BufferDesc {
    usize sizeBytes = 0;
    BufferUsage usage = BufferUsage::None;
    std::string_view debugName{}; // Shown in RenderDoc and the GPU memory panel.
};

struct TextureDesc {
    TextureType type = TextureType::Texture2D;
    Format format = Format::RGBA8Unorm;
    u32 width = 1;
    u32 height = 1;
    u32 layers = 1; // Array layers for Texture2DArray.
    u32 mipLevels = 1;
    TextureUsage usage = TextureUsage::Sampled;
    std::string_view debugName{};
};

struct SamplerDesc {
    Filter minFilter = Filter::Linear;
    Filter magFilter = Filter::Linear;
    MipmapMode mipmapMode = MipmapMode::Linear;
    AddressMode addressU = AddressMode::Repeat;
    AddressMode addressV = AddressMode::Repeat;
    f32 maxAnisotropy = 1.0F; // 1 disables anisotropic filtering.
    std::string_view debugName{};
};

struct ShaderDesc {
    ShaderStage stage = ShaderStage::Vertex;
    // Complete GLSL source, usually from preprocessShader (shader_source.hpp).
    std::string_view source{};
    // The file behind each source string number in the #line directives, so
    // compile errors can name "include/bindings.glsl:12" instead of "1(12)".
    // Optional; logs keep the raw numbers without it.
    std::span<const std::string> fileNames{};
    std::string_view debugName{};
};

inline constexpr usize kMaxColorAttachments = 8;

struct BlendState {
    bool enabled = false;
    BlendFactor srcColor = BlendFactor::One;
    BlendFactor dstColor = BlendFactor::Zero;
    BlendOp colorOp = BlendOp::Add;
    BlendFactor srcAlpha = BlendFactor::One;
    BlendFactor dstAlpha = BlendFactor::Zero;
    BlendOp alphaOp = BlendOp::Add;

    friend constexpr bool operator==(const BlendState&, const BlendState&) = default;

    // Classic alpha blending for translucent geometry and UI: color weighted
    // by the source alpha, alpha accumulated so the target stays usable.
    [[nodiscard]] static constexpr BlendState alphaBlend() noexcept {
        return BlendState{.enabled = true,
                          .srcColor = BlendFactor::SrcAlpha,
                          .dstColor = BlendFactor::OneMinusSrcAlpha,
                          .colorOp = BlendOp::Add,
                          .srcAlpha = BlendFactor::One,
                          .dstAlpha = BlendFactor::OneMinusSrcAlpha,
                          .alphaOp = BlendOp::Add};
    }
};

struct RasterState {
    PolygonMode polygonMode = PolygonMode::Fill;
    CullMode cullMode = CullMode::Back;
    FrontFace frontFace = FrontFace::CounterClockwise;
    bool depthClamp = false;

    friend constexpr bool operator==(const RasterState&, const RasterState&) = default;
};

struct DepthState {
    bool testEnabled = true;
    bool writeEnabled = true;
    CompareOp compare = CompareOp::GreaterOrEqual; // Reversed Z (ADR-010).

    friend constexpr bool operator==(const DepthState&, const DepthState&) = default;
};

// An immutable pipeline: shaders plus every piece of fixed function state.
// Either `compute` is set, or `vertex` (and usually `fragment`).
struct PipelineDesc {
    ShaderHandle vertex{};
    ShaderHandle fragment{};
    ShaderHandle compute{};
    PrimitiveTopology topology = PrimitiveTopology::Triangles;
    RasterState raster{};
    DepthState depth{};
    std::array<BlendState, kMaxColorAttachments> blend{};
    // The formats of the targets the pipeline draws into. Must match the
    // render pass it is used in (checked in debug builds).
    std::array<Format, kMaxColorAttachments> colorFormats{};
    u32 colorCount = 0;
    Format depthFormat = Format::Undefined;
    std::string_view debugName{};
};

struct ColorAttachment {
    TextureHandle texture;  // Ignored when `swapchain` is set.
    bool swapchain = false; // Draw into the window's back buffer.
    LoadOp load = LoadOp::Clear;
    StoreOp store = StoreOp::Store;
    std::array<f32, 4> clearColor{0.0F, 0.0F, 0.0F, 1.0F};
};

struct DepthAttachment {
    TextureHandle texture{};
    LoadOp load = LoadOp::Clear;
    StoreOp store = StoreOp::Store;
    f32 clearDepth = 0.0F; // Reversed Z clears to 0 (ADR-010).
};

// A render pass: the targets a group of draws renders into. Plain data with
// fixed size arrays, so it can be recorded into a command list by memcpy.
struct RenderPassDesc {
    std::array<ColorAttachment, kMaxColorAttachments> colors{};
    u32 colorCount = 0;
    DepthAttachment depth{};
    bool hasDepth = false;
    // Render area. 0 width and height means "the whole target".
    i32 x = 0;
    i32 y = 0;
    u32 width = 0;
    u32 height = 0;
};

// Data for one mip level and array layer of a texture upload. Rows are
// tightly packed (width * bytesPerTexel bytes each).
struct TextureUpload {
    u32 mipLevel = 0;
    u32 layer = 0;
    std::span<const std::byte> data{};
};

} // namespace vxe::rhi
