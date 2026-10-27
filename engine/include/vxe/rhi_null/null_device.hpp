#pragma once

#include <vxe/rhi/device.hpp>

#include <memory>

// The Null backend (Section 7.6): a Device that needs no GPU. Every creation
// succeeds with a real handle after the same validation the OpenGL backend
// does, CpuWrite buffers get real CPU memory, and submit() only counts
// commands. Unit tests run renderer code against it, and a future dedicated
// server can run the engine without any GPU at all.
namespace vxe::rhi {

[[nodiscard]] std::unique_ptr<Device> createNullDevice();

} // namespace vxe::rhi
