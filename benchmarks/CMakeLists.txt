# Micro benchmarks with nanobench (Section 24). They are built with every
# preset so they keep compiling, and run by hand from a release build:
#   build/release/bin/vxe_benchmarks
# Results and their dates go to docs/perf/.

find_package(nanobench CONFIG REQUIRED)

add_executable(vxe_benchmarks
    main.cpp
    meshing_bench.cpp
    storage_bench.cpp
    world_bench.cpp
)
target_link_libraries(vxe_benchmarks PRIVATE vxe::vxe nanobench::nanobench)
# The synthetic terrain sample is shared with the unit tests.
target_include_directories(vxe_benchmarks PRIVATE "${PROJECT_SOURCE_DIR}/engine/tests")
vxe_set_warnings(vxe_benchmarks)
