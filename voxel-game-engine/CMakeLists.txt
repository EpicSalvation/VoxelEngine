project(voxel-game-engine)

cmake_minimum_required(VERSION 3.10)

set(CMAKE_CXX_STANDARD 17)

include_directories(include)

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