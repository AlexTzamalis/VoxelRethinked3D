// Entry point of Voxel Rethinked. For now it only proves that the game links
// the engine. Later this becomes the place that creates VoxelRethinkedApp and
// hands it to vxe::runApplication (Section 6.5 of the plan).
#include <vxe/core/version.hpp>

#include <iostream>

int main() {
    std::cout << "Voxel Rethinked " << vxe::engineVersion() << '\n';
    return 0;
}
