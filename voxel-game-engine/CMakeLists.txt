cmake_minimum_required(VERSION 3.10)

project(voxel-game-engine)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)

include_directories(include 
                    third_party/bgfx/include 
                    third_party/bx/include 
                    third_party/bimg/include
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

# Add Google Test
add_subdirectory(external/googletest)
enable_testing()
# Add a test executable
file(GLOB_RECURSE TEST_SOURCES tests/*.cpp)
add_executable(voxel-game-engine-tests ${TEST_SOURCES} ${SOURCES})
target_include_directories(voxel-game-engine-tests PRIVATE include)
target_link_libraries(voxel-game-engine-tests gtest gtest_main)
# Add a test target
add_test(NAME AllTests COMMAND voxel-game-engine-tests)
add_executable(BgfxInitTest tests/BgfxInitTest.cpp)
target_include_directories(BgfxInitTest PRIVATE third_party/bgfx/include)
target_link_directories(BgfxInitTest PRIVATE third_party/bgfx/.build/osx-arm64/bin)
target_link_libraries(BgfxInitTest PRIVATE bgfxRelease)
