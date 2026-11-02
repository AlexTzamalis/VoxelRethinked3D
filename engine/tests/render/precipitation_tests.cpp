#include "../test_helpers.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/render/precipitation.hpp>
#include <vxe/rhi_null/null_device.hpp>

#include <doctest/doctest.h>

#include <array>
#include <vector>

using namespace vxe;
using render::PrecipitationKind;
using render::PrecipitationRenderer;

namespace {

render::PackImage streakImage() {
    render::PackImage image;
    image.width = 64;
    image.height = 256;
    image.pixels.assign(usize{64} * 256, 0xC0FFFFFFU);
    return image;
}

} // namespace

TEST_CASE("Rain only falls with the weather, an image and the setting on") {
    const auto device = rhi::createNullDevice();
    config::SettingsRegistry settings;
    auto created = PrecipitationRenderer::create({.device = device.get(),
                                                  .settings = &settings,
                                                  .shaders = rhi::folderShaderReader(VXE_ENGINE_SHADER_DIR)});
    REQUIRE(created.hasValue());
    PrecipitationRenderer& rain = *created.value();
    CHECK_FALSE(rain.isFalling());
    rain.setWeather(PrecipitationKind::Rain, 0.8F);
    CHECK_FALSE(rain.isFalling()); // No image yet.
    const render::PackImage image = streakImage();
    const std::array<const render::PackImage*, 2> images{&image, nullptr};
    rain.setImages(images);
    CHECK(rain.isFalling());
    rain.setWeather(PrecipitationKind::Snow, 0.8F);
    CHECK_FALSE(rain.isFalling()); // The pack has no snow image.
    rain.setWeather(PrecipitationKind::Rain, 0.0F);
    CHECK_FALSE(rain.isFalling());
    rain.setWeather(PrecipitationKind::Rain, 1.0F);
    REQUIRE(settings.set("Graphics.Sky.Precipitation", false).hasValue());
    CHECK_FALSE(rain.isFalling());
    REQUIRE(settings.set("Graphics.Sky.Precipitation", true).hasValue());

    // A frame with columns uploads and records without trouble.
    std::vector<i32> tops(usize{render::kPrecipitationSide} * render::kPrecipitationSide, 70);
    tops[5] = PrecipitationRenderer::kOpenColumn;
    rain.setColumns(-16, -16, tops);
    render::Camera camera;
    camera.position = DVec3(0.5, 90.0, 0.5);
    rain.update(camera, 12.0, Vec3(1.0F));
    rhi::CommandList commands;
    rain.record(commands);
    CHECK_FALSE(commands.empty());
}
