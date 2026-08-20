cmake_minimum_required(VERSION 3.25)

project(
    LunarTerrainBuilder
    VERSION 0.1.0
    DESCRIPTION "Deterministic engine-independent lunar terrain database tooling"
    LANGUAGES CXX
)

option(LUNAR_TERRAIN_BUILD_TESTING "Build Lunar Terrain tests" ON)
option(LUNAR_TERRAIN_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(LunarTerrainSanitizers)
include(LunarTerrainWarnings)

find_package(OpenSSL REQUIRED)
find_package(zstd CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(GDAL CONFIG REQUIRED)
find_package(PROJ CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)

if(TARGET zstd::libzstd_static)
    set(lunar_terrain_zstd_target zstd::libzstd_static)
elseif(TARGET zstd::libzstd_shared)
    set(lunar_terrain_zstd_target zstd::libzstd_shared)
else()
    message(FATAL_ERROR "The zstd package did not provide a supported imported target")
endif()

add_library(lunar_terrain_core)
add_library(LunarTerrain::Core ALIAS lunar_terrain_core)

target_sources(
    lunar_terrain_core
    PRIVATE
        src/core/digest.cpp
        src/core/error.cpp
        src/core/tile_key.cpp
    PUBLIC
        FILE_SET public_headers
        TYPE HEADERS
        BASE_DIRS include
        FILES
            include/lunar/terrain/coordinates.hpp
            include/lunar/terrain/digest.hpp
            include/lunar/terrain/error.hpp
            include/lunar/terrain/format.hpp
            include/lunar/terrain/format_v1.hpp
            include/lunar/terrain/ids.hpp
            include/lunar/terrain/result.hpp
            include/lunar/terrain/tile.hpp
            include/lunar/terrain/tile_key.hpp
)

target_compile_features(lunar_terrain_core PUBLIC cxx_std_20)
set_target_properties(lunar_terrain_core PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(lunar_terrain_core PRIVATE OpenSSL::Crypto ${lunar_terrain_zstd_target})
lunar_terrain_set_warnings(lunar_terrain_core)
lunar_terrain_enable_sanitizers(lunar_terrain_core)

add_library(lunar_terrain_builder_lib)
target_sources(
    lunar_terrain_builder_lib
    PRIVATE
        src/builder/builder.cpp
        src/builder/builder.hpp
)
target_include_directories(lunar_terrain_builder_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(lunar_terrain_builder_lib PUBLIC cxx_std_20)
set_target_properties(lunar_terrain_builder_lib PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(
    lunar_terrain_builder_lib
    PUBLIC LunarTerrain::Core
    PRIVATE
        GDAL::GDAL
        PROJ::proj
        unofficial::sqlite3::sqlite3
        tomlplusplus::tomlplusplus
        fmt::fmt
)
lunar_terrain_set_warnings(lunar_terrain_builder_lib)
lunar_terrain_enable_sanitizers(lunar_terrain_builder_lib)

add_executable(lunar-terrain src/cli/main.cpp)
target_compile_features(lunar-terrain PRIVATE cxx_std_20)
set_target_properties(lunar-terrain PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(lunar-terrain PRIVATE lunar_terrain_builder_lib CLI11::CLI11 fmt::fmt)
lunar_terrain_set_warnings(lunar-terrain)
lunar_terrain_enable_sanitizers(lunar-terrain)

if(LUNAR_TERRAIN_BUILD_TESTING)
    include(CTest)
    enable_testing()
    find_package(Catch2 3 CONFIG REQUIRED)
    include(Catch)

    add_executable(
        lunar_terrain_core_tests
        tests/core/contract_tests.cpp
        tests/core/format_v1_tests.cpp
        tests/core/tile_key_tests.cpp
    )
    target_compile_features(lunar_terrain_core_tests PRIVATE cxx_std_20)
    set_target_properties(lunar_terrain_core_tests PROPERTIES CXX_EXTENSIONS OFF)
    target_compile_definitions(
        lunar_terrain_core_tests
        PRIVATE LUNAR_TERRAIN_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/format_v1"
    )
    target_link_libraries(lunar_terrain_core_tests PRIVATE LunarTerrain::Core Catch2::Catch2WithMain)
    lunar_terrain_set_warnings(lunar_terrain_core_tests)
    lunar_terrain_enable_sanitizers(lunar_terrain_core_tests)
    catch_discover_tests(lunar_terrain_core_tests TEST_PREFIX "core.")

    add_executable(lunar_terrain_builder_tests tests/builder/builder_tests.cpp)
    target_compile_features(lunar_terrain_builder_tests PRIVATE cxx_std_20)
    set_target_properties(lunar_terrain_builder_tests PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(
        lunar_terrain_builder_tests
        PRIVATE lunar_terrain_builder_lib Catch2::Catch2WithMain
    )
    lunar_terrain_set_warnings(lunar_terrain_builder_tests)
    lunar_terrain_enable_sanitizers(lunar_terrain_builder_tests)
    catch_discover_tests(lunar_terrain_builder_tests TEST_PREFIX "builder.")

    add_test(NAME cli.version COMMAND lunar-terrain --version)
    set_tests_properties(cli.version PROPERTIES PASS_REGULAR_EXPRESSION "lunar-terrain 0.1.0")
endif()
