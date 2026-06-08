cmake_minimum_required(VERSION 3.16)
project(voxel-game-engine)

# bgfx's bx library requires C++20 (designated initializers in its SIMD headers).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Some pinned dependencies (yaml-cpp 0.8.0, glm 1.0.1) declare a
# cmake_minimum_required below 3.5. Modern CMake has removed compatibility with
# those policy versions and errors out. This raises the effective minimum for
# such subprojects so they configure under newer CMake.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# bgfx.cmake — CMake wrapper for bgfx/bimg/bx maintained by bgfx's author.
# Provides the bgfx, bimg, bx CMake targets.
# Pinned to a specific release tag for reproducible builds.
set(BGFX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
# Building shaders from source needs bgfx's shaderc, which unconditionally links
# tint/Dawn (bgfx's WebGPU backend) — thousands of translation units that blow
# past CI runner limits. Shaders are therefore precompiled and committed
# (shaders/generated/); the toolchain is built only when explicitly regenerating
# them. See the "Shaders" section below and docs/ARCHITECTURE.md §9.
option(VOXEL_BUILD_SHADERS "Build shaderc and recompile shaders from source" OFF)
if(VOXEL_BUILD_SHADERS)
  set(BGFX_BUILD_TOOLS          ON  CACHE BOOL "" FORCE)
  set(BGFX_BUILD_TOOLS_SHADER   ON  CACHE BOOL "" FORCE)
  set(BGFX_BUILD_TOOLS_GEOMETRY OFF CACHE BOOL "" FORCE)
  set(BGFX_BUILD_TOOLS_TEXTURE  OFF CACHE BOOL "" FORCE)
else()
  set(BGFX_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(
  bgfx_cmake
  GIT_REPOSITORY https://github.com/bkaradzic/bgfx.cmake.git
  GIT_TAG        v1.143.9257-544
  GIT_SUBMODULES_RECURSE TRUE
)
FetchContent_MakeAvailable(bgfx_cmake)

# GLM — math library required by WorldCoord (dvec3 / vec3)
FetchContent_Declare(
  glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG        1.0.1
)
FetchContent_MakeAvailable(glm)

# yaml-cpp — YAML parser for LayerConfig
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  yaml-cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG        0.8.0
)
FetchContent_MakeAvailable(yaml-cpp)

# GLFW — windowing and input. bgfx renders into the native window handle GLFW
# creates; GLFW itself creates no graphics context (the renderer sets
# GLFW_NO_API). On Linux this pulls X11 (and optionally Wayland) dev libraries.
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG        3.4
)
FetchContent_MakeAvailable(glfw)

# GoogleTest
set(gtest_SOURCE_DIR "${CMAKE_SOURCE_DIR}/external/googletest")
if(EXISTS "${gtest_SOURCE_DIR}/CMakeLists.txt")
  message(STATUS "Using local GoogleTest from ${gtest_SOURCE_DIR}")
  add_subdirectory(${gtest_SOURCE_DIR} external/googletest_build)
else()
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        main
  )
  FetchContent_MakeAvailable(googletest)
endif()

# ─── Engine library ──────────────────────────────────────────────────
# The engine is a library; games and demos are separate executables that
# link it (see demos/ below). Builds static by default; pass
# -DBUILD_SHARED_LIBS=ON to produce a shared library instead (the plugin
# ABI is unaffected either way — plugins receive a function-pointer table,
# see include/plugin_api.h).
file(GLOB_RECURSE ENGINE_SOURCES
    src/core/*.cpp
    src/io/*.cpp
    src/platform/*.cpp
    src/plugins/*.cpp
    src/renderer/*.cpp
    src/simulation/*.cpp
    src/world/*.cpp
)

add_library(voxel-engine ${ENGINE_SOURCES})

# include/ is the public API (WorldCoord.h, plugin_api.h) and propagates to
# consumers; src/ holds engine internals and is private to the library's own
# compilation. In-tree demos/tests opt into src/ explicitly when they need it.
target_include_directories(voxel-engine
  PUBLIC  include
  PRIVATE src
)

target_link_libraries(voxel-engine
  PUBLIC
    glm::glm              # glm types appear in public headers (WorldCoord)
    ${CMAKE_DL_LIBS}      # dlopen/dlsym/dlclose (libdl on Linux; empty elsewhere)
  PRIVATE
    bgfx                  # renderer internals only; not exposed in the public API
    bx
    bimg
    glfw                  # windowing/input; native handles wrapped by src/platform
    yaml-cpp::yaml-cpp
)

# Warning flags applied to first-party targets only (not fetched dependencies).
function(voxel_set_warnings tgt)
  if(MSVC)
    target_compile_options(${tgt} PRIVATE /W4 /MP)
    target_compile_definitions(${tgt} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

voxel_set_warnings(voxel-engine)

# ─── Shaders ─────────────────────────────────────────────────────────
# Shaders are authored in shaders/*.sc and precompiled to per-backend bytecode
# headers committed under shaders/generated/. Normal builds (and CI) consume the
# committed headers and need no shader toolchain. BgfxRenderer includes the
# per-backend headers and selects the right one at runtime via
# bgfx::createEmbeddedShader. <generated/<profile>/<name>.sc.bin.h> resolves here:
target_include_directories(voxel-engine PRIVATE ${CMAKE_SOURCE_DIR}/shaders)

# Opt-in regeneration. After editing a shader:
#   cmake -B build -DVOXEL_BUILD_SHADERS=ON
#   cmake --build build --target voxel-shaders   # then commit shaders/generated/
# All backends are produced from one host (shaderc cross-compiles), so the
# committed set covers every platform: SPIR-V/GLSL/ESSL plus DXBC (Direct3D11)
# and Metal. Direct3D12 (DXIL) and WebGPU (WGSL) are intentionally omitted —
# see docs/ARCHITECTURE.md §9.
if(VOXEL_BUILD_SHADERS)
  set(BGFX_SHADER_INCLUDE_PATH ${bgfx_cmake_SOURCE_DIR}/bgfx/src)
  set(VOXEL_SHADER_GEN ${CMAKE_SOURCE_DIR}/shaders/generated)
  # Synchronized rows: target platform | shaderc profile | output dir | array suffix
  set(_voxel_plats    linux  linux android windows osx)
  set(_voxel_profiles spirv  120   100_es  s_5_0   metal)
  set(_voxel_pdirs    spirv  glsl  essl    dxbc    metal)
  set(_voxel_exts     spv    glsl  essl    dxbc    mtl)
  list(LENGTH _voxel_profiles _voxel_n)
  math(EXPR _voxel_last "${_voxel_n} - 1")
  set(_voxel_shader_outputs)
  foreach(_stage "vertex|vs_voxel" "fragment|fs_voxel")
    string(REPLACE "|" ";" _stage_pair "${_stage}")
    list(GET _stage_pair 0 _stage_type)
    list(GET _stage_pair 1 _stage_name)
    foreach(_i RANGE ${_voxel_last})
      list(GET _voxel_plats    ${_i} _plat)
      list(GET _voxel_profiles ${_i} _prof)
      list(GET _voxel_pdirs    ${_i} _pdir)
      list(GET _voxel_exts     ${_i} _ext)
      set(_out ${VOXEL_SHADER_GEN}/${_pdir}/${_stage_name}.sc.bin.h)
      add_custom_command(
        OUTPUT  ${_out}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${VOXEL_SHADER_GEN}/${_pdir}
        COMMAND bgfx::shaderc --type ${_stage_type} --platform ${_plat} -p ${_prof} -O 3
                --varyingdef ${CMAKE_SOURCE_DIR}/shaders/varying.def.sc
                -f ${CMAKE_SOURCE_DIR}/shaders/${_stage_name}.sc
                -o ${_out} --bin2c ${_stage_name}_${_ext}
                -i ${BGFX_SHADER_INCLUDE_PATH}
        MAIN_DEPENDENCY ${CMAKE_SOURCE_DIR}/shaders/${_stage_name}.sc
        DEPENDS ${CMAKE_SOURCE_DIR}/shaders/varying.def.sc
        VERBATIM
      )
      list(APPEND _voxel_shader_outputs ${_out})
    endforeach()
  endforeach()
  add_custom_target(voxel-shaders DEPENDS ${_voxel_shader_outputs})
endif()

# ─── Plugins ─────────────────────────────────────────────────────────
# Runtime-loadable plugins, each built as a MODULE shared library from
# plugins/<name>/plugin.cpp and loaded via PluginManager::loadPlugin at runtime.
# Plugins link ZERO engine symbols (see docs/architecture.md §12): they see only
# the public API in include/ — plus src/ for the in-tree Voxel definition a
# generator fills — and receive a function-pointer table (PluginContext). All
# plugin binaries land in one folder under the build tree. Adding a plugin is
# just dropping in plugins/<name>/plugin.cpp — no edits here are needed.
set(VOXEL_PLUGIN_OUTDIR ${CMAKE_BINARY_DIR}/plugins)
file(GLOB _voxel_plugin_dirs RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/plugins
     ${CMAKE_CURRENT_SOURCE_DIR}/plugins/*)
list(SORT _voxel_plugin_dirs)
set(VOXEL_PLUGIN_TARGETS)
foreach(_plug ${_voxel_plugin_dirs})
  set(_plug_dir  ${CMAKE_CURRENT_SOURCE_DIR}/plugins/${_plug})
  set(_plug_main ${_plug_dir}/plugin.cpp)
  if(IS_DIRECTORY ${_plug_dir} AND EXISTS ${_plug_main})
    add_library(${_plug} MODULE ${_plug_main})
    target_include_directories(${_plug} PRIVATE include src)
    target_link_libraries(${_plug} PRIVATE glm::glm)
    # PREFIX "" so the artifact is e.g. water.so (not libwater.so) on every OS.
    set_target_properties(${_plug} PROPERTIES
      PREFIX ""
      LIBRARY_OUTPUT_DIRECTORY ${VOXEL_PLUGIN_OUTDIR}
      RUNTIME_OUTPUT_DIRECTORY ${VOXEL_PLUGIN_OUTDIR})
    voxel_set_warnings(${_plug})
    list(APPEND VOXEL_PLUGIN_TARGETS ${_plug})
  endif()
endforeach()

# ─── Demos ───────────────────────────────────────────────────────────
# The demos form a progressive series of reference examples, each a standalone
# executable linking the engine library. Every immediate subdirectory of demos/
# that contains a main.cpp becomes its own build target named after the folder
# (e.g. demos/01-single-voxel/ → target 01-single-voxel). Adding a demo is just
# dropping in demos/<NN-name>/main.cpp — no edits here are needed. The numeric
# prefix keeps the series ordered and maps to the README milestones. As in-tree
# demos they are allowed to reach engine internals under src/.
file(GLOB _voxel_demo_dirs RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/demos
     ${CMAKE_CURRENT_SOURCE_DIR}/demos/*)
list(SORT _voxel_demo_dirs)
foreach(_demo ${_voxel_demo_dirs})
  set(_demo_dir  ${CMAKE_CURRENT_SOURCE_DIR}/demos/${_demo})
  set(_demo_main ${_demo_dir}/main.cpp)
  if(IS_DIRECTORY ${_demo_dir} AND EXISTS ${_demo_main})
    add_executable(${_demo} ${_demo_main})
    target_link_libraries(${_demo} PRIVATE voxel-engine bgfx bx glfw)
    target_include_directories(${_demo} PRIVATE src)
    voxel_set_warnings(${_demo})
    # The plugin-driven demo loads these from disk; build them first and inject
    # their per-config paths. Harmless no-ops for demos that don't use them.
    if(VOXEL_PLUGIN_TARGETS)
      add_dependencies(${_demo} ${VOXEL_PLUGIN_TARGETS})
    endif()
    if(TARGET base-terrain)
      target_compile_definitions(${_demo} PRIVATE
        VOXEL_BASE_PLUGIN_PATH="$<TARGET_FILE:base-terrain>")
    endif()
    if(TARGET water)
      target_compile_definitions(${_demo} PRIVATE
        VOXEL_WATER_PLUGIN_PATH="$<TARGET_FILE:water>")
    endif()
    if(TARGET layered-world)
      target_compile_definitions(${_demo} PRIVATE
        VOXEL_LAYERED_PLUGIN_PATH="$<TARGET_FILE:layered-world>")
    endif()
    if(TARGET arena)
      target_compile_definitions(${_demo} PRIVATE
        VOXEL_ARENA_PLUGIN_PATH="$<TARGET_FILE:arena>")
    endif()
    if(TARGET hazards)
      target_compile_definitions(${_demo} PRIVATE
        VOXEL_HAZARDS_PLUGIN_PATH="$<TARGET_FILE:hazards>")
    endif()
    # Inject the assets directory path for demos that bundle asset files.
    set(_demo_assets ${_demo_dir}/assets)
    if(EXISTS ${_demo_assets})
      target_compile_definitions(${_demo} PRIVATE
        DEMO_ASSET_DIR="${_demo_assets}")
    endif()
  endif()
endforeach()

# ─── Tests ───────────────────────────────────────────────────────────
enable_testing()

# Fixture plugins for the PluginManager disk-load tests — real MODULE shared
# libraries built from tests/fixtures/*.cpp. Excluded from the test executable's
# own sources below; their on-disk paths are injected as compile definitions.
set(VOXEL_TEST_PLUGINS)
foreach(_fx good badinit nosym)
  set(_fx_src ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/${_fx}_plugin.cpp)
  if(EXISTS ${_fx_src})
    add_library(test-plugin-${_fx} MODULE ${_fx_src})
    target_include_directories(test-plugin-${_fx} PRIVATE include src)
    target_link_libraries(test-plugin-${_fx} PRIVATE glm::glm)
    set_target_properties(test-plugin-${_fx} PROPERTIES
      PREFIX ""
      LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test-plugins
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test-plugins)
    voxel_set_warnings(test-plugin-${_fx})
    list(APPEND VOXEL_TEST_PLUGINS test-plugin-${_fx})
  endif()
endforeach()

file(GLOB_RECURSE TEST_SOURCES tests/*.cpp)
list(FILTER TEST_SOURCES EXCLUDE REGEX "/fixtures/")
if(TEST_SOURCES)
  add_executable(voxel-engine-tests ${TEST_SOURCES})
  target_link_libraries(voxel-engine-tests PRIVATE voxel-engine gtest gtest_main)
  target_include_directories(voxel-engine-tests PRIVATE src)
  voxel_set_warnings(voxel-engine-tests)
  if(VOXEL_TEST_PLUGINS)
    add_dependencies(voxel-engine-tests ${VOXEL_TEST_PLUGINS})
    target_compile_definitions(voxel-engine-tests PRIVATE
      VOXEL_FIXTURE_GOOD="$<TARGET_FILE:test-plugin-good>"
      VOXEL_FIXTURE_BADINIT="$<TARGET_FILE:test-plugin-badinit>"
      VOXEL_FIXTURE_NOSYM="$<TARGET_FILE:test-plugin-nosym>")
  endif()
  add_test(NAME AllTests COMMAND voxel-engine-tests)
endif()
