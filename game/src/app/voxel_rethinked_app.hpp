#pragma once

#include <vxe/app/application.hpp>

namespace vr {

// The game application (Section 20.1). For now it only proves that the game
// runs inside the engine loop: it opens the window and quits when the window
// is closed. The state machine (Boot, MainMenu, InGame ...) arrives in G0.1.
class VoxelRethinkedApp final : public vxe::Application {
public:
    vxe::Result<void> onInit(vxe::EngineContext& context) override;
    void onFrame(vxe::EngineContext& context, const vxe::FrameTime& time) override;
    void onShutdown(vxe::EngineContext& context) override;
};

} // namespace vr
