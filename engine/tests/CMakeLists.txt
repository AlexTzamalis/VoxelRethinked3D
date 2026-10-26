# Engine unit tests. One executable holds every engine test so the test run
# stays fast and the doctest main is compiled once.

find_package(doctest CONFIG REQUIRED)

add_executable(vxe_tests
    test_helpers.hpp
    test_main.cpp
    core/assert_tests.cpp
    core/containers_tests.cpp
    core/log_tests.cpp
    core/memory_tests.cpp
    core/result_tests.cpp
)
target_link_libraries(vxe_tests PRIVATE vxe::vxe doctest::doctest)
# doctest forward declares std types by default; with real std headers it can
# print std::string_view and other values in failure messages.
target_compile_definitions(vxe_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
vxe_set_warnings(vxe_tests)

# Registered as a single CTest test. doctest returns a non zero exit code when
# any test case fails, which is all CTest needs.
add_test(NAME vxe_tests COMMAND vxe_tests)
