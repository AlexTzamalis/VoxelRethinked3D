// vxe_benchmarks: runs every micro benchmark and prints nanobench's tables.
// Each benchmark file adds a run function below. Run from a release build;
// debug numbers mean nothing. One argument picks a group: storage, world,
// meshing, region, light, lod or ui.

#include "benchmarks.hpp"

#include <cstdio>
#include <exception>
#include <string_view>

int main(int argc, char** argv) {
    // An optional argument runs one group only, for example "ui".
    const std::string_view only = argc > 1 ? std::string_view(argv[1]) : std::string_view();
    const auto wanted = [only](std::string_view group) { return only.empty() || only == group; };
    try {
        if (wanted("storage")) {
            vxe::bench::runStorageBenchmarks();
        }
        if (wanted("world")) {
            vxe::bench::runWorldBenchmarks();
        }
        if (wanted("meshing")) {
            vxe::bench::runMeshingBenchmarks();
        }
        if (wanted("region")) {
            vxe::bench::runRegionBenchmarks();
        }
        if (wanted("light")) {
            vxe::bench::runLightBenchmarks();
        }
        if (wanted("noise")) {
            vxe::bench::runNoiseBenchmarks();
        }
        if (wanted("lod")) {
            vxe::bench::runLodBenchmarks();
        }
        if (wanted("ui")) {
            vxe::bench::runUiBenchmarks();
        }
        return 0;
    } catch (const std::exception& error) {
        std::fputs("vxe_benchmarks: ", stderr);
        std::fputs(error.what(), stderr);
        std::fputs("\n", stderr);
    } catch (...) {
        std::fputs("vxe_benchmarks: unknown failure\n", stderr);
    }
    return 1;
}
