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

# Raise bgfx's static buffer-handle caps above the stock 4096. These are
# compile-time constants inside bgfx (BGFX_CONFIG_MAX_*; the matching Caps::Limits
# fields are read-only and there is no bgfx::Init knob), so they must be defined on
# the bgfx target itself. The asteroid-belt miner (demos/17) decomposes solid
# bodies into a deep 1 m grid; at large approach radii its peak resident non-empty
# chunk count — each chunk costs one static vertex + one static index buffer —
# exceeds 4096, which would otherwise exhaust the pools and drop geometry.
target_compile_definitions(bgfx PRIVATE
  BGFX_CONFIG_MAX_VERTEX_BUFFERS=16384
  BGFX_CONFIG_MAX_INDEX_BUFFERS=16384)

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

# ENet — lightweight UDP networking library (MIT), transport backend for M11.
# ENet v1.3.17 ships no CMakeLists.txt, so we populate and build it manually.
FetchContent_Declare(
  enet
  GIT_REPOSITORY https://github.com/lsalzman/enet.git
  GIT_TAG        v1.3.17
)
FetchContent_GetProperties(enet)
if(NOT enet_POPULATED)
  FetchContent_Populate(enet)
  # Build the platform-correct set of C files.
  set(ENET_SOURCES
    ${enet_SOURCE_DIR}/callbacks.c
    ${enet_SOURCE_DIR}/compress.c
    ${enet_SOURCE_DIR}/host.c
    ${enet_SOURCE_DIR}/list.c
    ${enet_SOURCE_DIR}/packet.c
    ${enet_SOURCE_DIR}/peer.c
    ${enet_SOURCE_DIR}/protocol.c
  )
  if(WIN32)
    list(APPEND ENET_SOURCES ${enet_SOURCE_DIR}/win32.c)
  else()
    list(APPEND ENET_SOURCES ${enet_SOURCE_DIR}/unix.c)
  endif()
  add_library(enet STATIC ${ENET_SOURCES})
  target_include_directories(enet PUBLIC "${enet_SOURCE_DIR}/include")
  if(WIN32)
    target_link_libraries(enet PRIVATE ws2_32 winmm)
  else()
    # HAS_SOCKLEN_T suppresses ENet's `typedef int socklen_t` which conflicts
    # with the POSIX definition already provided by <sys/socket.h> on Linux/macOS.
    target_compile_definitions(enet PRIVATE HAS_SOCKLEN_T=1)
  endif()
  # Suppress warnings in fetched code.
  set_target_properties(enet PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
    "${enet_SOURCE_DIR}/include")
endif()

# miniaudio — single-header audio library (public-domain/MIT-0) for M12.
# Downloaded as a single file; no git clone needed for a header-only library.
# MiniaudioBackend.cpp is the ONLY file that includes it (MINIAUDIO_IMPLEMENTATION).
# No miniaudio type appears in any public header (include/) — same PRIVATE rule as
# bgfx and ENet (ARCHITECTURE §12/§16).
set(MINIAUDIO_VERSION "0.11.21")
set(MINIAUDIO_DIR "${CMAKE_BINARY_DIR}/_miniaudio")
set(MINIAUDIO_HEADER "${MINIAUDIO_DIR}/miniaudio.h")
if(NOT EXISTS "${MINIAUDIO_HEADER}")
  message(STATUS "Downloading miniaudio ${MINIAUDIO_VERSION}...")
  file(MAKE_DIRECTORY "${MINIAUDIO_DIR}")
  file(DOWNLOAD
    "https://raw.githubusercontent.com/mackron/miniaudio/${MINIAUDIO_VERSION}/miniaudio.h"
    "${MINIAUDIO_HEADER}"
    STATUS _ma_status
    TLS_VERIFY ON
  )
  list(GET _ma_status 0 _ma_code)
  if(NOT _ma_code EQUAL 0)
    list(GET _ma_status 1 _ma_msg)
    message(FATAL_ERROR "Failed to download miniaudio.h: ${_ma_msg}")
  endif()
  message(STATUS "miniaudio downloaded to ${MINIAUDIO_HEADER}")
endif()
# Expose miniaudio's directory as a PRIVATE include so only engine internals see it.

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
    src/audio/*.cpp
    src/core/*.cpp
    src/io/*.cpp
    src/net/*.cpp
    src/platform/*.cpp
    src/plugins/*.cpp
    src/renderer/*.cpp
    src/simulation/*.cpp
    src/world/*.cpp
)

add_library(voxel-engine ${ENGINE_SOURCES})

# include/ is the public API and propagates to consumers; src/ holds engine
# internals and is private to the library's own compilation. The committed
# public surface is WorldCoord.h, plugin_api.h, the consumer front-end types
# (core/Engine.h, core/LayerConfig.h), and the renderer seam (renderer/Renderer.h
# + renderer/RendererFactory.h, with platform/NativeWindowHandles.h) — the
# concrete bgfx renderer stays under src/ behind createRenderer() (§12).
# In-tree demos/tests opt into src/ explicitly when they need engine internals.
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
    bimg_decode           # image decode (PNG/JPEG/…) for the M15 texture atlas
    glfw                  # windowing/input; native handles wrapped by src/platform
    yaml-cpp::yaml-cpp
    enet                  # M11 networking transport (ENet types must not appear in include/)
)

# miniaudio: header-only, so only the include path is needed (no static library).
# MiniaudioBackend.cpp is the sole consumer; PRIVATE keeps it out of include/.
target_include_directories(voxel-engine PRIVATE ${MINIAUDIO_DIR})

# Platform audio libraries required by miniaudio's device I/O layer (not needed
# for the null device / headless test path, but needed to link for real playback).
if(WIN32)
  target_link_libraries(voxel-engine PRIVATE ole32 user32)
  # winmm is already pulled in by ENet on Windows; no duplicate needed.
elseif(APPLE)
  target_link_libraries(voxel-engine PRIVATE
    "-framework CoreAudio"
    "-framework AudioToolbox"
    "-framework AudioUnit")
else()
  # Linux: try ALSA and/or PulseAudio; miniaudio picks whichever backend it finds.
  find_package(ALSA QUIET)
  if(ALSA_FOUND)
    target_link_libraries(voxel-engine PRIVATE ALSA::ALSA)
  endif()
  find_library(PULSE_LIB pulse)
  if(PULSE_LIB)
    target_link_libraries(voxel-engine PRIVATE ${PULSE_LIB})
  endif()
  target_link_libraries(voxel-engine PRIVATE m pthread)
endif()

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
    if(TARGET material-showcase)
      target_compile_definitions(${_demo} PRIVATE
        VOXEL_SHOWCASE_PLUGIN_PATH="$<TARGET_FILE:material-showcase>")
    endif()
    if(TARGET recipe-world)
      target_compile_definitions(${_demo} PRIVATE
        VOXEL_RECIPE_PLUGIN_PATH="$<TARGET_FILE:recipe-world>")
    endif()
    if(TARGET drill-world)
      target_compile_definitions(${_demo} PRIVATE
        VOXEL_DRILL_PLUGIN_PATH="$<TARGET_FILE:drill-world>")
    endif()
    if(TARGET server-authority)
      target_compile_definitions(${_demo} PRIVATE
        VOXEL_AUTHORITY_PLUGIN_PATH="$<TARGET_FILE:server-authority>")
    endif()
    if(TARGET chat)
      target_compile_definitions(${_demo} PRIVATE
        VOXEL_CHAT_PLUGIN_PATH="$<TARGET_FILE:chat>")
    endif()
    if(TARGET material-audio)
      target_compile_definitions(${_demo} PRIVATE
        VOXEL_MATERIAL_AUDIO_PLUGIN_PATH="$<TARGET_FILE:material-audio>")
    endif()
    if(TARGET crumble)
      target_compile_definitions(${_demo} PRIVATE
        VOXEL_CRUMBLE_PLUGIN_PATH="$<TARGET_FILE:crumble>")
    endif()
    if(TARGET falling-debris)
      target_compile_definitions(${_demo} PRIVATE
        VOXEL_FALLING_DEBRIS_PLUGIN_PATH="$<TARGET_FILE:falling-debris>")
    endif()
    if(TARGET flow)
      target_compile_definitions(${_demo} PRIVATE
        VOXEL_FLOW_PLUGIN_PATH="$<TARGET_FILE:flow>")
    endif()
    if(TARGET field-sources)
      target_compile_definitions(${_demo} PRIVATE
        VOXEL_FIELD_SOURCES_PLUGIN_PATH="$<TARGET_FILE:field-sources>")
    endif()
    if(TARGET blockbench)
      target_compile_definitions(${_demo} PRIVATE
        VOXEL_BLOCKBENCH_PLUGIN_PATH="$<TARGET_FILE:blockbench>")
    endif()
    if(TARGET floating-playspace)
      target_compile_definitions(${_demo} PRIVATE
        VOXEL_FLOATING_PLAYSPACE_PLUGIN_PATH="$<TARGET_FILE:floating-playspace>")
    endif()
    if(TARGET asteroid-field)
      target_compile_definitions(${_demo} PRIVATE
        VOXEL_ASTEROID_FIELD_PLUGIN_PATH="$<TARGET_FILE:asteroid-field>")
      # The asteroid-belt-miner demo shares the body lattice with the plugin
      # (asteroid_field.h), so it needs the plugin's dir plus include/ (for the
      # header-only RNG that header pulls in). Harmless for other demos.
      target_include_directories(${_demo} PRIVATE
        include ${CMAKE_CURRENT_SOURCE_DIR}/plugins/asteroid-field)
    endif()
    if(TARGET kinematic-body)
      target_compile_definitions(${_demo} PRIVATE
        VOXEL_KINEMATIC_BODY_PLUGIN_PATH="$<TARGET_FILE:kinematic-body>")
      # The kinematic-body plugin shares its API header (kinematic_body.h) with
      # demos that use it, mirroring the asteroid-field pattern.
      target_include_directories(${_demo} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/plugins/kinematic-body)
    endif()
    # Inject the assets directory path for demos that bundle asset files.
    set(_demo_assets ${_demo_dir}/assets)
    if(EXISTS ${_demo_assets})
      target_compile_definitions(${_demo} PRIVATE
        DEMO_ASSET_DIR="${_demo_assets}")
    endif()
    # Inject the repo root (which holds the top-level assets/ directory) so a demo
    # can fall back to it when launched from a directory that lacks assets/ — e.g.
    # 12-soundscape, whose material-audio plugin resolves sounds relative to cwd.
    target_compile_definitions(${_demo} PRIVATE
      VOXEL_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
  endif()
endforeach()

# ─── Tests ───────────────────────────────────────────────────────────
enable_testing()

# Fixture plugins for the PluginManager disk-load tests — real MODULE shared
# libraries built from tests/fixtures/*.cpp. Excluded from the test executable's
# own sources below; their on-disk paths are injected as compile definitions.
set(VOXEL_TEST_PLUGINS)
foreach(_fx good badinit nosym badabi)
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
  # KinematicBodyTest.cpp wires in the kinematic-body plugin directly (via
  # wireInPlugin), so its source is compiled into the test binary so the
  # kinbody::api() inline static resolves in one address space.
  set(_kb_plugin_src ${CMAKE_CURRENT_SOURCE_DIR}/plugins/kinematic-body/plugin.cpp)
  # The reference input plugins (M17 C1) are likewise compiled in so their
  # kbinput::api() / gpinput::api() inline statics resolve in one address space.
  set(_km_plugin_src ${CMAKE_CURRENT_SOURCE_DIR}/plugins/keyboard-mouse/plugin.cpp)
  set(_gp_plugin_src ${CMAKE_CURRENT_SOURCE_DIR}/plugins/gamepad/plugin.cpp)
  add_executable(voxel-engine-tests ${TEST_SOURCES}
    ${_kb_plugin_src} ${_km_plugin_src} ${_gp_plugin_src})
  # Suppress each plugin's generic voxel_plugin_init wrapper so they don't clash
  # with each other or with ExamplePlugin's voxel_plugin_init in the engine lib.
  set_source_files_properties(${_kb_plugin_src} PROPERTIES
    COMPILE_DEFINITIONS KINBODY_COMPILED_IN)
  set_source_files_properties(${_km_plugin_src} PROPERTIES
    COMPILE_DEFINITIONS KBINPUT_COMPILED_IN)
  set_source_files_properties(${_gp_plugin_src} PROPERTIES
    COMPILE_DEFINITIONS GPINPUT_COMPILED_IN)
  target_link_libraries(voxel-engine-tests PRIVATE voxel-engine gtest gtest_main)
  target_include_directories(voxel-engine-tests PRIVATE src)
  # AsteroidFieldTest.cpp checks the body lattice the asteroid-field plugin and
  # the demo share (plugins/asteroid-field/asteroid_field.h, header-only).
  # KinematicBodyTest.cpp uses the kinematic-body plugin's shared header.
  # InputPluginsTest.cpp uses the keyboard-mouse / gamepad shared headers.
  target_include_directories(voxel-engine-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/plugins/asteroid-field
    ${CMAKE_CURRENT_SOURCE_DIR}/plugins/kinematic-body
    ${CMAKE_CURRENT_SOURCE_DIR}/plugins/keyboard-mouse
    ${CMAKE_CURRENT_SOURCE_DIR}/plugins/gamepad)
  voxel_set_warnings(voxel-engine-tests)
  if(VOXEL_TEST_PLUGINS)
    add_dependencies(voxel-engine-tests ${VOXEL_TEST_PLUGINS})
    target_compile_definitions(voxel-engine-tests PRIVATE
      VOXEL_FIXTURE_GOOD="$<TARGET_FILE:test-plugin-good>"
      VOXEL_FIXTURE_BADINIT="$<TARGET_FILE:test-plugin-badinit>"
      VOXEL_FIXTURE_NOSYM="$<TARGET_FILE:test-plugin-nosym>"
      VOXEL_FIXTURE_BADABI="$<TARGET_FILE:test-plugin-badabi>")
  endif()
  # The M9 recipe tests load the recipe-world plugin to exercise its real
  # cave/ore feature generators through the registered FeatureGeneratorFn.
  if(TARGET recipe-world)
    add_dependencies(voxel-engine-tests recipe-world)
    target_compile_definitions(voxel-engine-tests PRIVATE
      VOXEL_RECIPE_PLUGIN_PATH="$<TARGET_FILE:recipe-world>")
  endif()
  # The M16 resolve_noise test loads base-terrain / layered-world to confirm
  # their generators consume the built-in noise registry through resolve_noise.
  if(TARGET base-terrain)
    add_dependencies(voxel-engine-tests base-terrain)
    target_compile_definitions(voxel-engine-tests PRIVATE
      VOXEL_BASE_PLUGIN_PATH="$<TARGET_FILE:base-terrain>")
  endif()
  if(TARGET layered-world)
    add_dependencies(voxel-engine-tests layered-world)
    target_compile_definitions(voxel-engine-tests PRIVATE
      VOXEL_LAYERED_PLUGIN_PATH="$<TARGET_FILE:layered-world>")
  endif()
  # The M15 Blockbench import test loads the blockbench plugin to exercise its
  # real .bbmodel importer through the registered ImporterFn.
  if(TARGET blockbench)
    add_dependencies(voxel-engine-tests blockbench)
    target_compile_definitions(voxel-engine-tests PRIVATE
      VOXEL_BLOCKBENCH_PLUGIN_PATH="$<TARGET_FILE:blockbench>")
  endif()
  add_test(NAME AllTests COMMAND voxel-engine-tests)
endif()
