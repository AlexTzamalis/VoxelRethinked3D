// Commits to one region file in an endless loop until it is killed. The
// region crash test starts it, kills it at a random moment and then checks
// the file still opens with one complete commit (Section 21 crash safety).
// Each commit writes slots 0 to 63 with payloads of the commit's round.

#include "../voxel/region_test_payload.hpp"

#include <vxe/voxel/region_file.hpp>

#include <cstdio>
#include <exception>
#include <span>
#include <vector>

namespace {

int run(const char* path) {
    auto file = vxe::voxel::RegionFile::open(path, true);
    if (!file) {
        std::fputs(file.error().message().c_str(), stderr);
        return 2;
    }
    vxe::voxel::RegionFile& region = *file.value();
    std::vector<std::vector<std::byte>> payloads(64);
    std::vector<vxe::voxel::RegionFile::Change> changes(64);
    for (vxe::u64 round = region.sequence() + 1;; ++round) {
        for (vxe::u16 slot = 0; slot < 64; ++slot) {
            payloads[slot] = vxe::test::regionTestPayload(round, slot);
            changes[slot] = {.slot = slot, .payload = payloads[slot]};
        }
        if (!region.commit(changes, vxe::io::Codec::Zstd, 1)) {
            return 3;
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::span<char*> arguments(argv, static_cast<std::size_t>(argc));
    if (arguments.size() != 2) {
        std::fputs("usage: vxe_region_crash_writer <region file>\n", stderr);
        return 1;
    }
    try {
        return run(arguments[1]);
    } catch (const std::exception& error) {
        std::fputs(error.what(), stderr);
    }
    return 4;
}
