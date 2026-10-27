#include "voxel_rethinked_app.hpp"

#include <vxe/core/log.hpp>
#include <vxe/core/version.hpp>

namespace vr {
namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): the level changes at runtime.
constinit vxe::log::Category gameLog{"game", vxe::log::Level::Info};

} // namespace

vxe::Result<void> VoxelRethinkedApp::onInit(vxe::EngineContext& context) {
    vxe::log::registerCategory(gameLog);
    VXE_LOG_INFO(gameLog, "Voxel Rethinked {} started", vxe::engineVersion());
    if (context.window != nullptr) {
        context.window->setTitle("Voxel Rethinked");
    }
    return {};
}

void VoxelRethinkedApp::onFrame(vxe::EngineContext& /*context*/, const vxe::FrameTime& /*time*/) {
    // Nothing to simulate or draw yet; the renderer arrives in E8.
}

void VoxelRethinkedApp::onShutdown(vxe::EngineContext& /*context*/) {
    VXE_LOG_INFO(gameLog, "Voxel Rethinked shutting down");
}

} // namespace vr
