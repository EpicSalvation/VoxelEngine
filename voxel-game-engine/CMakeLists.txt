cmake_minimum_required(VERSION 3.10)

project(voxel-game-engine)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)

# Add Google Test
include(FetchContent)

# Helper function to use local path or fetch
function(use_local_or_fetch name path url)
  if(EXISTS ${CMAKE_SOURCE_DIR}/${path}/CMakeLists.txt)
    message(STATUS "Using local ${name} from ${path}")
    add_subdirectory(${CMAKE_SOURCE_DIR}/${path} ${path}-build)
  else()
    message(STATUS "Fetching ${name} from ${url}")
    FetchContent_Declare(
      ${name}
      GIT_REPOSITORY ${url}
      GIT_TAG master
    )
    FetchContent_MakeAvailable(${name})
  endif()
endfunction()

# Call it for bx, bimg, bgfx
use_local_or_fetch(bx    "third_party/bx"    https://github.com/bkaradzic/bx.git)
use_local_or_fetch(bimg  "third_party/bimg"  https://github.com/bkaradzic/bimg.git)
use_local_or_fetch(bgfx  "third_party/bgfx"  https://github.com/bkaradzic/bgfx.git)

include_directories(include 
                    third_party/bx/include 
                    third_party/bimg/include
                    third_party/bgfx/include 
)

# target_compile_definitions(BgfxInitTest PRIVATE 
#     $<$<CONFIG:Debug>:BX_CONFIG_DEBUG=1>
#     $<$<CONFIG:Release>:BX_CONFIG_DEBUG=0>
# )

file(GLOB_RECURSE SOURCES
    src/core/*.cpp
    src/plugins/*.cpp
    src/renderer/*.cpp
    src/world/*.cpp
    src/main.cpp
)

add_executable(voxel-game-engine ${SOURCES})

# Optionally, you can add any libraries or dependencies here
# target_link_libraries(voxel-game-engine <your_dependencies>)

# First, try to use local clone if present
set(gtest_SOURCE_DIR "${CMAKE_SOURCE_DIR}/external/googletest")
if(EXISTS "${gtest_SOURCE_DIR}/CMakeLists.txt")
  message(STATUS "Using local GoogleTest from ${gtest_SOURCE_DIR}")
  add_subdirectory(${gtest_SOURCE_DIR} external/googletest_build)
else()
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/heads/main.zip
  )
  FetchContent_MakeAvailable(googletest)
endif()

enable_testing()

# Add a test executable
# file(GLOB_RECURSE TEST_SOURCES tests/*.cpp)
# add_executable(voxel-game-engine-tests ${TEST_SOURCES} ${SOURCES})
# target_include_directories(voxel-game-engine-tests PRIVATE include)
# target_link_libraries(voxel-game-engine-tests gtest gtest_main)

# Add a test target
add_test(NAME AllTests COMMAND voxel-game-engine-tests)
add_executable(BgfxInitTest tests/BgfxInitTest.cpp)
target_include_directories(BgfxInitTest PRIVATE third_party/bgfx/include)
target_link_directories(BgfxInitTest PRIVATE third_party/bgfx/.build/osx-arm64/bin)
target_link_libraries(BgfxInitTest 
  PRIVATE
    bgfx
    bimg
    bx
)
# Set debug flag only in Debug builds
target_compile_definitions(BgfxInitTest PRIVATE
  $<$<CONFIG:Debug>:BGFX_CONFIG_DEBUG=1>
)
