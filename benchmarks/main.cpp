// vxe_benchmarks: runs every micro benchmark and prints nanobench's tables.
// Each benchmark file adds a run function below. Run from a release build;
// debug numbers mean nothing.

#include "benchmarks.hpp"

#include <cstdio>
#include <exception>

int main() {
    try {
        vxe::bench::runStorageBenchmarks();
        vxe::bench::runWorldBenchmarks();
        vxe::bench::runMeshingBenchmarks();
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
