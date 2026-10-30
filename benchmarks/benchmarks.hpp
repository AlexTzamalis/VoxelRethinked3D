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

// Sky and block light of a chunk (light_bench.cpp, E10.3).
void runLightBenchmarks();

// LOD tiles, their cache files and the clipmap (lod_bench.cpp, E11.1).
void runLodBenchmarks();

// Noise for world generation (noise_bench.cpp, G3.0).
void runNoiseBenchmarks();

// UI layout and drawing of a menu sized tree (ui_bench.cpp, E12.2).
void runUiBenchmarks();

} // namespace vxe::bench
