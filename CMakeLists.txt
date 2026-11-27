# Builds the mod with the Geode SDK: src/ and sim/ in one dll, without FMA or fast math so the simulator matches the game
# bit for bit, and with the bindings pinned to the last 2.2081 commit (the CI uses the same one).

cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(AutoPathingTool VERSION 1.0.1)

add_library(${PROJECT_NAME} SHARED
    src/main.cpp
    src/settings.cpp
    src/log.cpp
    src/player_state.cpp
    src/macro.cpp
    src/engine.cpp
    src/editor.cpp
    src/input_recorder.cpp
    src/simplanner.cpp
    src/world_export.cpp
    sim/collision.cpp
    sim/collision2.cpp
    sim/physics.cpp
    sim/step.cpp
    sim/rotation.cpp
    sim/slopes.cpp
    sim/world.cpp
    sim/recording.cpp
    sim/search.cpp
    sim/search_backoff.cpp
    sim/search_causes.cpp
    sim/search_costs.cpp
    sim/search_debt.cpp
    sim/search_expand.cpp
    sim/search_layer.cpp
    sim/search_maps.cpp
    sim/search_restart.cpp
    sim/timeline.cpp
    sim/triggers.cpp
    sim/triggers_analyze.cpp
    sim/triggers_area.cpp
    sim/triggers_blocks.cpp
    sim/triggers_commands.cpp
    sim/triggers_fire.cpp
    sim/triggers_follow.cpp
    sim/triggers_groups.cpp
    sim/triggers_items.cpp
    sim/triggers_move.cpp
    sim/triggers_positions.cpp
    sim/triggers_state.cpp
    sim/camera.cpp
    src/objwatch.cpp
    src/overlay.cpp
    src/recorder.cpp
    src/ui/menu.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE src)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        target_compile_options(${PROJECT_NAME} PRIVATE /clang:-ffp-contract=off /clang:-fno-fast-math)
    else()
        target_compile_options(${PROJECT_NAME} PRIVATE -ffp-contract=off -fno-fast-math)
    endif()
elseif (MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /fp:precise)
endif()

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

if (NOT DEFINED ENV{GEODE_BINDINGS_REPO_PATH} AND NOT GEODE_BINDINGS_REPO_PATH)
    include(FetchContent)
    FetchContent_Declare(ap_bindings
        GIT_REPOSITORY https://github.com/geode-sdk/bindings.git
        GIT_TAG 1605f500d053f8de122bf5c7dc6d17c24866e02e
        SOURCE_SUBDIR no-cmake-here
    )
    FetchContent_MakeAvailable(ap_bindings)
    set(GEODE_BINDINGS_REPO_PATH ${ap_bindings_SOURCE_DIR})
endif()
set(SKIP_BUILDING_CODEGEN OFF)

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

setup_geode_mod(${PROJECT_NAME})
