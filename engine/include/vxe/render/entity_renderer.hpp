#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>
#include <vxe/math/vector.hpp>
#include <vxe/render/cpu_write_ring.hpp>
#include <vxe/render/texture_pack.hpp>
#include <vxe/rhi/command_list.hpp>
#include <vxe/rhi/device.hpp>
#include <vxe/rhi/shader_source.hpp>

#include <array>
#include <memory>
#include <span>
#include <vector>

namespace vxe::render {

// Entity texture layers, all 64 x 64: the skin, then the six faces of the item
// in hand (+X, -X, +Y, -Y, +Z, -Z), each scaled up from its 16 pixels.
inline constexpr u32 kEntityTextureSize = 64;
inline constexpr u32 kEntitySkinLayer = 0;
inline constexpr u32 kEntityItemLayer = 1; // The first of six.
inline constexpr u32 kEntityTextureLayers = 7;
inline constexpr u32 kEntityMaxVertices = 8192;

// One corner of a triangle: camera relative position, texture coordinates in
// the 0 to 1 square of a layer, the layer, and the brightness (light times
// the face's shade). The layout must match EntityVertex in render/entity.vert.
struct EntityVertex {
    Vec4 position{0.0F}; // xyz camera relative, w brightness.
    Vec4 uv{0.0F};       // xy texture, z layer, w unused.
};
static_assert(sizeof(EntityVertex) == 32);

// Where a batch of entity triangles is drawn.
enum class EntityLayer : u8 {
    World,       // With the world: depth tested against terrain (other players, the body in third person).
    FirstPerson, // The hand and the item in it, over everything (drawn after the world).
};

struct EntityRendererDesc {
    rhi::Device* device = nullptr;
    rhi::ShaderFileReader shaders{};
    rhi::Format colorFormat = rhi::Format::RGBA16Float;
    rhi::Format depthFormat = rhi::Format::D32Float;
};

// Draws entities made of textured boxes (the player, G6 F5 views): whoever owns
// the entity builds its triangles on the CPU each frame, camera relative, and
// hands them in with add(); the renderer streams them to the GPU and draws them
// with alpha cutout (the second layer of a skin has holes). A few hundred
// triangles a frame, so building them on the CPU is cheaper than any GPU
// skinning setup.
//
// First person batches are pushed to the front of the depth range in the
// vertex shader, so the hand never sinks into a wall in front of it.
//
// Thread safety: main thread only.
class EntityRenderer {
public:
    [[nodiscard]] static Result<std::unique_ptr<EntityRenderer>> create(const EntityRendererDesc& desc);
    ~EntityRenderer();
    EntityRenderer(const EntityRenderer&) = delete;
    EntityRenderer& operator=(const EntityRenderer&) = delete;
    EntityRenderer(EntityRenderer&&) = delete;
    EntityRenderer& operator=(EntityRenderer&&) = delete;

    // The skin: a 64 x 64 image (Minecraft's layout). Null keeps a plain grey.
    void setSkin(const PackImage* image);
    // The item in hand: six 16 x 16 face images, RGBA with red lowest, top row
    // first (+X, -X, +Y, -Y, +Z, -Z).
    void setItemFaces(std::span<const std::vector<u32>, 6> faces);

    // Forgets last frame's triangles. Once per frame, before add().
    void beginFrame();
    // Adds triangles (three vertices each) for this frame. Vertices beyond
    // kEntityMaxVertices in total are dropped.
    void add(EntityLayer layer, std::span<const EntityVertex> vertices);
    // Uploads this frame's triangles; before recording.
    void upload();
    // Draws one layer's triangles (frame constants bound).
    void record(rhi::CommandList& commands, EntityLayer layer) const;

private:
    explicit EntityRenderer(rhi::Device& device) noexcept
        : m_device(device) {}
    void uploadLayer(u32 layer, std::span<const u32> pixels);

    rhi::Device& m_device;
    rhi::PipelineHandle m_world;
    rhi::PipelineHandle m_firstPerson;
    rhi::SamplerHandle m_sampler;
    rhi::TextureHandle m_textures;
    CpuWriteRing m_ring;
    std::array<std::vector<EntityVertex>, 2> m_vertices;
};

} // namespace vxe::render
