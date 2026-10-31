# Engine module helper and dependency rules (ADR-001, ADR-015).
#
# Every engine module is created with vxe_add_module(). The helper enforces at
# configure time that:
#   - a module only depends on modules in strictly lower layers,
#   - modules with an explicit allow list (net) only depend on those modules,
#   - graphics API libraries (glad, OpenGL) are linked privately and only by
#     rhi_gl, so no other target can even see their headers,
#   - no engine module links a game target,
#   - source files only include headers of modules they depend on, and graphics
#     API headers only appear in rhi_gl (CheckIncludes.cmake).
#
# The layer table below is the single source of truth for the engine layering.
# It is a strict total order of the layer diagram in Section 6.1 of the plan:
# boxes drawn side by side get the same layer, which forbids them from
# depending on each other.

include_guard(GLOBAL)
# Captured now: inside a function CMAKE_CURRENT_LIST_DIR is the caller's folder.
set(VXE_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")
include("${VXE_CMAKE_DIR}/CheckIncludes.cmake")

# Pairs of <module> <layer>. Lower numbers are lower layers.
set(VXE_MODULE_LAYERS
    core     0
    math     1
    jobs     2
    io       3
    platform 4
    net      4
    config   5
    input    6
    audio    6
    rhi      7
    rhi_gl   8
    rhi_null 8
    render   9
    ui       10
    debug    11
    voxel    12
    app      13
)

# Modules that may only depend on an explicit list, on top of the layer rule.
# net must stay usable by a headless dedicated server (Section 6.1).
set(VXE_MODULE_ONLY_net core jobs io)

# Libraries that expose a graphics API. Only rhi_gl may link them, privately.
set(VXE_GRAPHICS_API_LIBS glad glad::glad OpenGL::GL OpenGL::OpenGL opengl32)

# Writes the layer number of a module into outVar, or fails for unknown modules.
function(vxe_module_layer name outVar)
    list(FIND VXE_MODULE_LAYERS "${name}" index)
    # A match must be a module name (even index), not a layer number (odd index).
    math(EXPR isName "${index} % 2")
    if(index EQUAL -1 OR NOT isName EQUAL 0)
        message(FATAL_ERROR "Unknown engine module '${name}'. Add it to VXE_MODULE_LAYERS in cmake/VxeModule.cmake.")
    endif()
    math(EXPR layerIndex "${index} + 1")
    list(GET VXE_MODULE_LAYERS ${layerIndex} layer)
    set(${outVar} ${layer} PARENT_SCOPE)
endfunction()

# Fails configuration if module `name` is not allowed to depend on module `dep`.
# Kept free of target commands so the CMake script tests can call it directly.
function(vxe_check_module_dependency name dep)
    vxe_module_layer(${name} layer)
    vxe_module_layer(${dep} depLayer)
    if(NOT depLayer LESS layer)
        message(FATAL_ERROR
            "Engine module '${name}' (layer ${layer}) may not depend on '${dep}' (layer ${depLayer}). "
            "Modules may only depend on strictly lower layers (docs/adr/0015-module-layer-order.md).")
    endif()
    if(DEFINED VXE_MODULE_ONLY_${name} AND NOT dep IN_LIST VXE_MODULE_ONLY_${name})
        message(FATAL_ERROR
            "Engine module '${name}' may not depend on '${dep}'. "
            "It may only depend on: ${VXE_MODULE_ONLY_${name}}.")
    endif()
endfunction()

# Fails configuration if a third party library list breaks the graphics API or
# game separation rules. `visibility` is PUBLIC or PRIVATE.
function(vxe_check_module_libs name visibility)
    foreach(lib IN LISTS ARGN)
        if(lib IN_LIST VXE_GRAPHICS_API_LIBS)
            if(NOT name STREQUAL "rhi_gl")
                message(FATAL_ERROR "Engine module '${name}' links the graphics API library '${lib}'. Only rhi_gl may (ADR-003).")
            endif()
            if(NOT visibility STREQUAL "PRIVATE")
                message(FATAL_ERROR "rhi_gl must link '${lib}' PRIVATE so its headers never leak to other targets.")
            endif()
        endif()
        if(lib MATCHES "^(voxel_rethinked|vr_|vr::)")
            message(FATAL_ERROR "Engine module '${name}' links the game target '${lib}'. The engine never depends on the game (ADR-001).")
        endif()
    endforeach()
endfunction()

# Adds a build step that re-runs the include check whenever one of the files
# changes, so violations added after configuring still fail the build.
# Accepts the same arguments as vxe_check_includes() plus the target to guard.
function(vxe_add_include_check target)
    cmake_parse_arguments(arg "ANY_MODULE;ALLOW_GRAPHICS_API" "OWNER" "ALLOWED_MODULES;FILES" ${ARGN})

    set(flags "")
    if(arg_ANY_MODULE)
        list(APPEND flags ANY_MODULE)
    endif()
    if(arg_ALLOW_GRAPHICS_API)
        list(APPEND flags ALLOW_GRAPHICS_API)
    endif()
    # Check right away so a violation fails configuration, not only the build.
    vxe_check_includes(${flags} OWNER "${arg_OWNER}" ALLOWED_MODULES ${arg_ALLOWED_MODULES} FILES ${arg_FILES})

    set(checkDir "${CMAKE_CURRENT_BINARY_DIR}/include_checks")
    set(argsFile "${checkDir}/${target}.cmake")
    set(stampFile "${checkDir}/${target}.stamp")
    # Lists go into a generated file because semicolons do not survive a
    # custom command line reliably. Bracket arguments avoid any escaping.
    file(CONFIGURE OUTPUT "${argsFile}" CONTENT [==[
set(VXE_CHECK_OWNER [=[@arg_OWNER@]=])
set(VXE_CHECK_ALLOWED_MODULES [=[@arg_ALLOWED_MODULES@]=])
set(VXE_CHECK_FILES [=[@arg_FILES@]=])
set(VXE_CHECK_ANY_MODULE @arg_ANY_MODULE@)
set(VXE_CHECK_ALLOW_GRAPHICS_API @arg_ALLOW_GRAPHICS_API@)
]==] @ONLY)

    add_custom_command(
        OUTPUT "${stampFile}"
        COMMAND "${CMAKE_COMMAND}" "-DVXE_CHECK_ARGS_FILE=${argsFile}" "-DVXE_CHECK_STAMP=${stampFile}"
                -P "${VXE_CMAKE_DIR}/CheckIncludes.cmake"
        DEPENDS ${arg_FILES} "${argsFile}" "${VXE_CMAKE_DIR}/CheckIncludes.cmake"
        COMMENT "Checking include rules for ${target}"
        VERBATIM
    )
    add_custom_target(${target}_include_check DEPENDS "${stampFile}")
    add_dependencies(${target} ${target}_include_check)
endfunction()

# Creates the static library vxe_<name> with the alias vxe::<name>.
#   SOURCES          Source files, relative to the calling CMakeLists.txt.
#   DEPENDS          Engine modules used in public headers (linked PUBLIC).
#   PRIVATE_DEPENDS  Engine modules used only in sources (linked PRIVATE).
#   PUBLIC_LIBS      Third party targets used in public headers.
#   PRIVATE_LIBS     Third party targets used only in sources.
# Public headers are picked up from include/vxe/<name>/ automatically.
# Dependencies must be created before the modules that use them, which the
# layer order guarantees when modules are added from the lowest layer up.
function(vxe_add_module name)
    cmake_parse_arguments(arg "" "" "SOURCES;DEPENDS;PRIVATE_DEPENDS;PUBLIC_LIBS;PRIVATE_LIBS" ${ARGN})

    vxe_module_layer(${name} layer)
    foreach(dep IN LISTS arg_DEPENDS arg_PRIVATE_DEPENDS)
        vxe_check_module_dependency(${name} ${dep})
        if(NOT TARGET vxe_${dep})
            message(FATAL_ERROR "Engine module '${name}' depends on '${dep}', which is not defined yet. Add modules from the lowest layer up.")
        endif()
    endforeach()
    vxe_check_module_libs(${name} PUBLIC ${arg_PUBLIC_LIBS})
    vxe_check_module_libs(${name} PRIVATE ${arg_PRIVATE_LIBS})

    set(sources "")
    foreach(source IN LISTS arg_SOURCES)
        cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" OUTPUT_VARIABLE absolute)
        list(APPEND sources "${absolute}")
    endforeach()
    file(GLOB_RECURSE headers CONFIGURE_DEPENDS "${VXE_ENGINE_INCLUDE_DIR}/vxe/${name}/*.hpp")

    add_library(vxe_${name} STATIC ${sources} ${headers})
    add_library(vxe::${name} ALIAS vxe_${name})
    target_include_directories(vxe_${name} PUBLIC "${VXE_ENGINE_INCLUDE_DIR}")
    target_compile_features(vxe_${name} PUBLIC cxx_std_20)
    list(TRANSFORM arg_DEPENDS PREPEND "vxe::" OUTPUT_VARIABLE publicModules)
    list(TRANSFORM arg_PRIVATE_DEPENDS PREPEND "vxe::" OUTPUT_VARIABLE privateModules)
    target_link_libraries(vxe_${name}
        PUBLIC ${publicModules} ${arg_PUBLIC_LIBS}
        PRIVATE ${privateModules} ${arg_PRIVATE_LIBS})
    vxe_set_warnings(vxe_${name})

    # The modules whose headers this module may include: itself plus the full
    # dependency closure. Stored so modules above can build their own closure.
    set(closure ${name})
    foreach(dep IN LISTS arg_DEPENDS arg_PRIVATE_DEPENDS)
        get_property(depClosure GLOBAL PROPERTY VXE_MODULE_CLOSURE_${dep})
        list(APPEND closure ${depClosure})
    endforeach()
    list(REMOVE_DUPLICATES closure)
    set_property(GLOBAL PROPERTY VXE_MODULE_CLOSURE_${name} ${closure})
    set_property(GLOBAL APPEND PROPERTY VXE_MODULES ${name})

    set(graphicsFlag "")
    if(name STREQUAL "rhi_gl")
        set(graphicsFlag ALLOW_GRAPHICS_API)
    endif()
    vxe_add_include_check(vxe_${name} ${graphicsFlag}
        OWNER "module ${name}"
        ALLOWED_MODULES ${closure}
        FILES ${sources} ${headers})
endfunction()

# Creates the INTERFACE target vxe (alias vxe::vxe) that links every engine
# module added so far. The game and tools link this one target. Call it after
# the last vxe_add_module().
function(vxe_add_engine_target)
    get_property(modules GLOBAL PROPERTY VXE_MODULES)
    list(TRANSFORM modules PREPEND "vxe::")
    add_library(vxe INTERFACE)
    add_library(vxe::vxe ALIAS vxe)
    target_link_libraries(vxe INTERFACE ${modules})
endfunction()
