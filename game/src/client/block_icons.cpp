#include "block_icons.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace vr {
namespace {

using vxe::f32;
using vxe::u32;
using vxe::usize;
using vxe::voxel::BlockFace;

// The cube's corners in icon pixels: a diamond for the top, two sides below.
constexpr f32 kCenterX = 16.0F;
constexpr f32 kTopY = 1.0F;
constexpr f32 kHalfWidth = 14.0F;
constexpr f32 kHalfDiamond = 7.0F;
constexpr f32 kSideHeight = 16.0F;

// Brightness of each side, in 256ths, so the three faces read as a cube.
constexpr u32 kTopShade = 256;
constexpr u32 kLeftShade = 200;
constexpr u32 kRightShade = 150;

u32 multiply(u32 color, u32 tint) {
    u32 result = color & 0xFF000000U;
    for (const u32 shift : {0U, 8U, 16U}) {
        result |= ((((color >> shift) & 255U) * ((tint >> shift) & 255U)) / 255U) << shift;
    }
    return result;
}

u32 shaded(u32 color, u32 shade) {
    u32 result = color & 0xFF000000U;
    for (const u32 shift : {0U, 8U, 16U}) {
        result |= (((color >> shift) & 255U) * shade / 256U) << shift;
    }
    return result;
}

// A pixel of an image at u and v from 0 to 1. Animated textures are strips of
// square frames; the first frame stands for them.
u32 sample(const vxe::render::PackImage& image, f32 u, f32 v) {
    const u32 size = image.width;
    const u32 x = std::min(static_cast<u32>(u * static_cast<f32>(size)), size - 1);
    const u32 y = std::min(static_cast<u32>(v * static_cast<f32>(size)), size - 1);
    return image.pixels[(usize{y} * size) + x];
}

// What a texture layer shows at u and v, with its tint and overlay.
u32 lookAt(const BlockCatalog& catalog, const BlockIcons::FindTexture& find, vxe::u16 layer, f32 u, f32 v) {
    const auto keys = catalog.textureKeys();
    const auto looks = catalog.textureLooks();
    const auto tints = catalog.tintColors();
    const vxe::render::PackImage* image = find(keys[layer]);
    if (image == nullptr || image->width == 0) {
        return 0xFFFF00FFU; // The missing texture's magenta.
    }
    const u32 look = looks[layer];
    u32 color = multiply(sample(*image, u, v), tints[(look >> 16U) & 15U]);
    if (const u32 overlayLayer = look & 0xFFFFU; overlayLayer != 0) {
        const vxe::render::PackImage* overlay = find(keys[overlayLayer - 1]);
        if (overlay != nullptr && overlay->width != 0) {
            const u32 top = multiply(sample(*overlay, u, v), tints[(look >> 20U) & 15U]);
            if ((top >> 24U) >= 128U) {
                color = (top & 0x00FFFFFFU) | 0xFF000000U;
            }
        }
    }
    return color;
}

} // namespace

void BlockIcons::bake(const BlockCatalog& catalog, const FindTexture& find) {
    m_icons.clear();
    m_iconOfState.assign(catalog.stateCount(), 0);
    for (const BlockDefinition& block : catalog.blocks()) {
        const vxe::voxel::BlockRenderInfo& info = catalog.renderInfo(block.firstState);
        if (!info.isVisible()) {
            continue;
        }
        if (info.layer == vxe::voxel::RenderLayer::Plant) {
            bakePlant(catalog, find, block, info);
            continue;
        }
        const auto layerTop = info.textureLayer(BlockFace::PosY);
        const auto layerLeft = info.textureLayer(BlockFace::PosZ);
        const auto layerRight = info.textureLayer(BlockFace::PosX);
        Icon icon{.name = "icon/" + block.name, .pixels = std::vector<u32>(usize{kSize} * kSize, 0U)};
        for (u32 y = 0; y < kSize; ++y) {
            for (u32 x = 0; x < kSize; ++x) {
                const f32 px = static_cast<f32>(x) + 0.5F - kCenterX;
                const f32 py = static_cast<f32>(y) + 0.5F - kTopY;
                u32 color = 0;
                // Diamond coordinates: 0 to 1 along its two edges.
                const f32 a = (px / kHalfWidth + py / kHalfDiamond) / 2.0F;
                const f32 b = (-px / kHalfWidth + py / kHalfDiamond) / 2.0F;
                if (a >= 0.0F && a < 1.0F && b >= 0.0F && b < 1.0F) {
                    color = shaded(lookAt(catalog, find, layerTop, a, b), kTopShade);
                } else {
                    // On a side, the height below the diamond's lower edges.
                    const f32 side = std::abs(px) / kHalfWidth;
                    const f32 drop = py - (2.0F * kHalfDiamond) + (side * kHalfDiamond);
                    const f32 v = drop / kSideHeight;
                    if (side < 1.0F && v >= 0.0F && v < 1.0F) {
                        const f32 u = px < 0.0F ? 1.0F - side : side;
                        color = px < 0.0F
                                    ? shaded(lookAt(catalog, find, layerLeft, u, v), kLeftShade)
                                    : shaded(lookAt(catalog, find, layerRight, 1.0F - u, v), kRightShade);
                    }
                }
                icon.pixels[(usize{y} * kSize) + x] = color;
            }
        }
        m_icons.push_back(std::move(icon));
        const auto index = static_cast<u32>(m_icons.size());
        for (vxe::u16 state = 0; state < block.stateCount; ++state) {
            m_iconOfState.at(usize{block.firstState} + state) = index;
        }
    }
}

// Plants are flat pictures, not cubes. A tall plant shows both halves.
void BlockIcons::bakePlant(const BlockCatalog& catalog, const FindTexture& find, const BlockDefinition& block,
                           const vxe::voxel::BlockRenderInfo& info) {
    Icon icon{.name = "icon/" + block.name, .pixels = std::vector<u32>(usize{kSize} * kSize, 0U)};
    const auto layer = info.textureLayer(BlockFace::PosX);
    const vxe::voxel::BlockStateId upper = catalog.tallUpper(block.firstState);
    const auto upperLayer = catalog.renderInfo(upper).textureLayer(BlockFace::PosX);
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            const f32 u = (static_cast<f32>(x) + 0.5F) / static_cast<f32>(kSize);
            const f32 v = (static_cast<f32>(y) + 0.5F) / static_cast<f32>(kSize);
            u32 color = 0;
            if (upper == vxe::voxel::kAirState) {
                color = lookAt(catalog, find, layer, u, v);
            } else if (v < 0.5F) {
                color = lookAt(catalog, find, upperLayer, u, v * 2.0F);
            } else {
                color = lookAt(catalog, find, layer, u, (v - 0.5F) * 2.0F);
            }
            icon.pixels[(usize{y} * kSize) + x] = color;
        }
    }
    m_icons.push_back(std::move(icon));
    m_iconOfState.at(block.firstState) = static_cast<u32>(m_icons.size());
}

std::string_view BlockIcons::iconOf(vxe::voxel::BlockStateId state) const {
    if (state >= m_iconOfState.size() || m_iconOfState[state] == 0) {
        return {};
    }
    return m_icons[m_iconOfState[state] - 1].name;
}

} // namespace vr
