// Entry point of Voxel Rethinked. For now it only proves that the game links
// the engine. Later this becomes the place that creates VoxelRethinkedApp and
// hands it to vxe::runApplication (Section 6.5 of the plan).
#include <vxe/core/version.hpp>

#include <cstdio>

int main() {
    const std::string_view version = vxe::engineVersion();
    std::printf("Voxel Rethinked %.*s\n", static_cast<int>(version.size()), version.data());
    return 0;
}
