#include <vxe/config/settings.hpp>
#include <vxe/render/water.hpp>
#include <vxe/rhi_null/null_device.hpp>

#include <doctest/doctest.h>

using namespace vxe;
using render::WaterRenderer;

TEST_CASE("Water fogs only with the eye under water, and its settings switch it") {
    const auto device = rhi::createNullDevice();
    config::SettingsRegistry settings;
    auto created = WaterRenderer::create({.device = device.get(),
                                          .settings = &settings,
                                          .shaders = rhi::folderShaderReader(VXE_ENGINE_SHADER_DIR)});
    REQUIRE(created.hasValue());
    WaterRenderer& water = *created.value();
    CHECK(settings.value("Graphics.Water.DepthColor").hasValue());
    CHECK(settings.value("Graphics.Water.Clarity").hasValue());

    // In the air: the depth copy and the surfaces' bindings, no fog.
    water.update(Vec3(1.0F), 1.0F);
    rhi::CommandList air;
    water.recordDepthCopy(air, rhi::TextureHandle{});
    water.bindSurfaces(air, rhi::TextureHandle{});
    CHECK_FALSE(water.isFogging());
    rhi::CommandList noFog;
    water.recordFog(noFog, rhi::TextureHandle{});
    CHECK(noFog.empty());

    // Under water: fog, until the setting turns it off.
    water.setView({.underWater = true, .depth = 12.0F, .color = Vec3(0.05F, 0.2F, 0.6F)});
    water.update(Vec3(1.0F), 1.0F);
    CHECK(water.isFogging());
    rhi::CommandList fog;
    water.recordFog(fog, rhi::TextureHandle{});
    CHECK_FALSE(fog.empty());
    REQUIRE(settings.set("Graphics.Water.UnderwaterFog", false).hasValue());
    CHECK_FALSE(water.isFogging());
}
