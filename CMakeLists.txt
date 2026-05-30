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
# Tools are needed for build-time shader compilation: shaderc (.sc -> bytecode)
# and bin2c (bytecode -> embeddable C headers). The texture/geometry tools are
# not used, so keep them off to limit build time.
set(BGFX_BUILD_TOOLS          ON  CACHE BOOL "" FORCE)
set(BGFX_BUILD_TOOLS_SHADER   ON  CACHE BOOL "" FORCE)
set(BGFX_BUILD_TOOLS_BIN2C    ON  CACHE BOOL "" FORCE)
set(BGFX_BUILD_TOOLS_GEOMETRY OFF CACHE BOOL "" FORCE)
set(BGFX_BUILD_TOOLS_TEXTURE  OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  bgfx_cmake
  GIT_REPOSITORY https://github.com/bkaradzic/bgfx.cmake.git
  GIT_TAG        v1.143.9257-544
  GIT_SUBMODULES_RECURSE TRUE
)
FetchContent_MakeAvailable(bgfx_cmake)

# bgfx.cmake's build-time shader compilation helpers (bgfx_compile_shaders).
include(${bgfx_cmake_SOURCE_DIR}/cmake/bgfxToolUtils.cmake)
# Under FetchContent (vs. an installed bgfx package) this is not set, but the
# shader helper needs it to resolve <bgfx_shader.sh> and friends.
set(BGFX_SHADER_INCLUDE_PATH ${bgfx_cmake_SOURCE_DIR}/bgfx/src)

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
    src/platform/*.cpp
    src/plugins/*.cpp
    src/renderer/*.cpp
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
# Compiled at build time by shaderc into per-backend bytecode, emitted as
# embeddable C headers (AS_HEADERS) under the build tree — no binaries are
# checked in. BgfxRenderer includes these and selects the right variant at
# runtime via bgfx::createEmbeddedShader. The default profile set per platform
# (spirv/glsl/essl plus dxbc+dxil on Windows, metal on macOS) matches the
# backends bgfx auto-selects.
set(VOXEL_SHADER_DIR ${CMAKE_BINARY_DIR}/generated/shaders)

# On Windows, pin the profile set to exclude s_6_0 (Direct3D12/DXIL). shaderc's
# DXC path rejects its own generated varying-init under the helper's hardcoded
# --werror; bgfx prefers Direct3D11 (s_5_0/DXBC) on Windows regardless, so D3D12
# is a later follow-up. Other platforms use the helper's defaults.
set(VOXEL_SHADER_PROFILES "")
if(WIN32)
  set(VOXEL_SHADER_PROFILES PROFILES spirv 120 100_es s_5_0)
endif()

bgfx_compile_shaders(
  TYPE        VERTEX
  SHADERS     ${CMAKE_SOURCE_DIR}/shaders/vs_voxel.sc
  VARYING_DEF ${CMAKE_SOURCE_DIR}/shaders/varying.def.sc
  OUTPUT_DIR  ${VOXEL_SHADER_DIR}
  OUT_FILES_VAR VOXEL_VS_OUTPUTS
  AS_HEADERS
  ${VOXEL_SHADER_PROFILES}
)
bgfx_compile_shaders(
  TYPE        FRAGMENT
  SHADERS     ${CMAKE_SOURCE_DIR}/shaders/fs_voxel.sc
  VARYING_DEF ${CMAKE_SOURCE_DIR}/shaders/varying.def.sc
  OUTPUT_DIR  ${VOXEL_SHADER_DIR}
  OUT_FILES_VAR VOXEL_FS_OUTPUTS
  AS_HEADERS
  ${VOXEL_SHADER_PROFILES}
)
add_custom_target(voxel-shaders DEPENDS ${VOXEL_VS_OUTPUTS} ${VOXEL_FS_OUTPUTS})
add_dependencies(voxel-engine voxel-shaders)
# Generated headers are included as <generated/shaders/<profile>/<name>.sc.bin.h>.
target_include_directories(voxel-engine PRIVATE ${CMAKE_BINARY_DIR})

# ─── Demos ───────────────────────────────────────────────────────────
# Each demo is a thin executable linking the engine library. 'sandbox' is the
# development launcher (formerly src/main.cpp). As an in-tree demo it is
# allowed to reach engine internals under src/.
add_executable(sandbox demos/sandbox/main.cpp)
target_link_libraries(sandbox PRIVATE voxel-engine)
target_include_directories(sandbox PRIVATE src)
voxel_set_warnings(sandbox)

# ─── Tests ───────────────────────────────────────────────────────────
enable_testing()

file(GLOB_RECURSE TEST_SOURCES tests/*.cpp)
if(TEST_SOURCES)
  add_executable(voxel-engine-tests ${TEST_SOURCES})
  target_link_libraries(voxel-engine-tests PRIVATE voxel-engine gtest gtest_main)
  target_include_directories(voxel-engine-tests PRIVATE src)
  voxel_set_warnings(voxel-engine-tests)
  add_test(NAME AllTests COMMAND voxel-engine-tests)
endif()
