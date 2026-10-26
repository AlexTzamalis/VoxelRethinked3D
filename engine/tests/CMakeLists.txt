# Engine unit tests. One executable holds every engine test so the test run
# stays fast and the doctest main is compiled once.

find_package(doctest CONFIG REQUIRED)

add_executable(vxe_tests
    test_main.cpp
)
target_link_libraries(vxe_tests PRIVATE vxe::core doctest::doctest)
vxe_set_warnings(vxe_tests)

# Registered as a single CTest test. doctest returns a non zero exit code when
# any test case fails, which is all CTest needs.
add_test(NAME vxe_tests COMMAND vxe_tests)
