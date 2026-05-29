cmake_minimum_required(VERSION 3.16)
project(voxel-game-engine)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# bgfx.cmake — CMake wrapper for bgfx/bimg/bx maintained by bgfx's author.
# Provides the bgfx, bimg, bx CMake targets.
# Pin to a specific release tag in production; 'master' is used here for bootstrap.
set(BGFX_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
set(BGFX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  bgfx_cmake
  GIT_REPOSITORY https://github.com/bkaradzic/bgfx.cmake.git
  GIT_TAG        master
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

file(GLOB_RECURSE SOURCES
    src/core/*.cpp
    src/plugins/*.cpp
    src/renderer/*.cpp
    src/world/*.cpp
    src/main.cpp
)

add_executable(voxel-game-engine ${SOURCES})

# Both include/ (public API headers) and src/ (engine internals) are on the path.
target_include_directories(voxel-game-engine PRIVATE include src)

target_link_libraries(voxel-game-engine
  PRIVATE
    bgfx
    bx
    bimg
    glm::glm
    yaml-cpp::yaml-cpp
)

enable_testing()

# Tests: uncomment when tests/ contains test sources and add engine sources without main.cpp.
# file(GLOB_RECURSE TEST_SOURCES tests/*.cpp)
# file(GLOB_RECURSE ENGINE_SOURCES src/core/*.cpp src/world/*.cpp src/plugins/*.cpp)
# add_executable(voxel-game-engine-tests ${TEST_SOURCES} ${ENGINE_SOURCES})
# target_include_directories(voxel-game-engine-tests PRIVATE include src)
# target_link_libraries(voxel-game-engine-tests PRIVATE gtest gtest_main glm::glm yaml-cpp::yaml-cpp)
# add_test(NAME AllTests COMMAND voxel-game-engine-tests)
