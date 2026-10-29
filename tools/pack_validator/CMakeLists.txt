# The texture pack validator (G2.4, Section 15.4): checks a pack folder with
# render::validatePack and exits non zero on errors.

add_executable(vxe_pack_validator main.cpp)
target_link_libraries(vxe_pack_validator PRIVATE vxe::vxe)
vxe_set_warnings(vxe_pack_validator)

if(VXE_BUILD_TESTS)
    # The default pack must always validate without errors.
    add_test(NAME pack_validator_default_pack
        COMMAND vxe_pack_validator "${PROJECT_SOURCE_DIR}/textures/default")
endif()
