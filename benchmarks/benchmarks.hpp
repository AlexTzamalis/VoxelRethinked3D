#pragma once

// The benchmark groups of vxe_benchmarks, one function per file.
namespace vxe::bench {

// Chunk storage (storage_bench.cpp, E9.2).
void runStorageBenchmarks();

// Voxel world and snapshots (world_bench.cpp, E9.3).
void runWorldBenchmarks();

// Binary greedy meshing (meshing_bench.cpp, E9.4).
void runMeshingBenchmarks();

// Chunk payloads and region files (region_bench.cpp, E9.8).
void runRegionBenchmarks();

} // namespace vxe::bench
