# Registers the build rule script tests with CTest. Each case runs
# module_rules_test.cmake in script mode and passes when its output matches the
# expected text. Matching the message (instead of only "CMake failed") proves
# the right rule fired, not an unrelated scripting error.

function(vxe_add_rule_test caseName expectedOutput)
    set(workDir "${CMAKE_CURRENT_BINARY_DIR}/work")
    file(MAKE_DIRECTORY "${workDir}")
    add_test(NAME cmake_rules.${caseName}
        COMMAND "${CMAKE_COMMAND}" "-DCASE=${caseName}" "-DWORK_DIR=${workDir}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/module_rules_test.cmake")
    set_tests_properties(cmake_rules.${caseName} PROPERTIES PASS_REGULAR_EXPRESSION "${expectedOutput}")
endfunction()

vxe_add_rule_test(valid_dependencies          "VXE_RULE_TEST_PASSED")
vxe_add_rule_test(upward_dependency           "'core' [(]layer 0[)] may not depend on 'render' [(]layer 9[)]")
vxe_add_rule_test(same_layer_dependency       "'rhi_null' [(]layer 8[)] may not depend on 'rhi_gl' [(]layer 8[)]")
vxe_add_rule_test(net_allow_list              "'net' may not depend on 'math'")
vxe_add_rule_test(unknown_module              "Unknown engine module 'banana'")
vxe_add_rule_test(valid_libs                  "VXE_RULE_TEST_PASSED")
vxe_add_rule_test(graphics_lib_outside_rhi_gl "'render' links the graphics API library 'glad::glad'")
vxe_add_rule_test(graphics_lib_public         "rhi_gl must link 'glad::glad' PRIVATE")
vxe_add_rule_test(engine_links_game           "links the game target 'voxel_rethinked'")
vxe_add_rule_test(valid_includes              "VXE_RULE_TEST_PASSED")
vxe_add_rule_test(include_of_higher_module    "module core includes a header of module 'render'")
vxe_add_rule_test(include_of_glad             "graphics API header outside rhi_gl")
vxe_add_rule_test(game_includes_gl            "graphics API header outside rhi_gl")
