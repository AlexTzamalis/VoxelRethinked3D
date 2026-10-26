#include <vxe/core/profile.hpp>

#include <doctest/doctest.h>

#include <thread>

namespace {

int profiledWork(int input) {
    VXE_PROFILE_FUNCTION();
    int sum = 0;
    for (int i = 0; i < input; ++i) {
        VXE_PROFILE_SCOPE("Inner step");
        sum += i;
    }
    VXE_PROFILE_PLOT("Profile test sum", static_cast<double>(sum));
    return sum;
}

} // namespace

TEST_CASE("Profiling macros compile and do not change behavior") {
    CHECK(profiledWork(10) == 45);
    VXE_PROFILE_FRAME();
    VXE_PROFILE_MESSAGE("profile test", 12);
}

TEST_CASE("isEnabled matches the build configuration") {
#if VXE_ENABLE_PROFILING
    CHECK(vxe::profiling::isEnabled());
#else
    CHECK_FALSE(vxe::profiling::isEnabled());
#endif
}

TEST_CASE("Threads can be named") {
    std::thread worker([] { vxe::profiling::setThreadName("Test worker with a long name"); });
    worker.join();
    vxe::profiling::setThreadName("Test main");
}
