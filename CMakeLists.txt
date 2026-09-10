cmake_minimum_required(VERSION 3.25)

project(
    LunarTerrainBuilder
    VERSION 0.3.0
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
        src/core/byte_io.cpp
        src/core/codec.cpp
        src/core/database.cpp
        src/core/digest.cpp
        src/core/error.cpp
        src/core/integrity.cpp
        src/core/qsc_projection.cpp
        src/core/qsc_topology.cpp
        src/core/tile_key.cpp
    PUBLIC
        FILE_SET public_headers
        TYPE HEADERS
        BASE_DIRS include
        FILES
            include/lunar/terrain/byte_io.hpp
            include/lunar/terrain/codec.hpp
            include/lunar/terrain/coordinates.hpp
            include/lunar/terrain/database.hpp
            include/lunar/terrain/digest.hpp
            include/lunar/terrain/error.hpp
            include/lunar/terrain/format.hpp
            include/lunar/terrain/format_v1.hpp
            include/lunar/terrain/ids.hpp
            include/lunar/terrain/integrity.hpp
            include/lunar/terrain/qsc_projection.hpp
            include/lunar/terrain/qsc_topology.hpp
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
        src/builder/build_cache.cpp
        src/builder/build_cache.hpp
        src/builder/configuration.cpp
        src/builder/configuration.hpp
        src/builder/container_writer.cpp
        src/builder/diagnostics.cpp
        src/builder/fusion.cpp
        src/builder/fusion.hpp
        src/builder/hierarchy.cpp
        src/builder/hierarchy.hpp
        src/builder/operators.cpp
        src/builder/packing.cpp
        src/builder/packing.hpp
        src/builder/raster_source.cpp
        src/builder/raster_source.hpp
        src/builder/tile_staging.cpp
        src/builder/tile_staging.hpp
        src/builder/task_executor.cpp
        src/builder/task_executor.hpp
        src/builder/telemetry.cpp
        src/builder/telemetry.hpp
)
target_include_directories(lunar_terrain_builder_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(lunar_terrain_builder_lib PUBLIC cxx_std_20)
set_target_properties(lunar_terrain_builder_lib PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(
    lunar_terrain_builder_lib
    PUBLIC LunarTerrain::Core
    PRIVATE
        GDAL::GDAL
        OpenSSL::Crypto
        PROJ::proj
        unofficial::sqlite3::sqlite3
        tomlplusplus::tomlplusplus
        fmt::fmt
        ${lunar_terrain_zstd_target}
)
if(WIN32)
    target_link_libraries(lunar_terrain_builder_lib PRIVATE Psapi)
endif()
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
    find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)
    include(Catch)

    add_executable(
        lunar_terrain_core_tests
        tests/core/byte_codec_tests.cpp
        tests/core/contract_tests.cpp
        tests/core/database_tests.cpp
        tests/core/format_v1_tests.cpp
        tests/core/qsc_tests.cpp
        tests/core/tile_key_tests.cpp
    )
    target_compile_features(lunar_terrain_core_tests PRIVATE cxx_std_20)
    set_target_properties(lunar_terrain_core_tests PROPERTIES CXX_EXTENSIONS OFF)
    target_compile_definitions(
        lunar_terrain_core_tests
        PRIVATE LUNAR_TERRAIN_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/format_v1"
    )
    target_link_libraries(
        lunar_terrain_core_tests
        PRIVATE LunarTerrain::Core Catch2::Catch2WithMain ${lunar_terrain_zstd_target}
    )
    lunar_terrain_set_warnings(lunar_terrain_core_tests)
    lunar_terrain_enable_sanitizers(lunar_terrain_core_tests)
    catch_discover_tests(lunar_terrain_core_tests TEST_PREFIX "core.")

    add_executable(
        lunar_terrain_builder_tests
        tests/builder/builder_tests.cpp
        tests/builder/build_cache_tests.cpp
        tests/builder/fusion_tests.cpp
        tests/builder/hierarchy_tests.cpp
        tests/builder/operator_tests.cpp
        tests/builder/packing_tests.cpp
        tests/builder/projection_oracle_tests.cpp
        tests/builder/raster_source_tests.cpp
        tests/builder/telemetry_tests.cpp
        tests/builder/tile_staging_tests.cpp
    )
    target_compile_features(lunar_terrain_builder_tests PRIVATE cxx_std_20)
    set_target_properties(lunar_terrain_builder_tests PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(
        lunar_terrain_builder_tests
        PRIVATE lunar_terrain_builder_lib Catch2::Catch2WithMain GDAL::GDAL PROJ::proj
    )
    lunar_terrain_set_warnings(lunar_terrain_builder_tests)
    lunar_terrain_enable_sanitizers(lunar_terrain_builder_tests)
    catch_discover_tests(lunar_terrain_builder_tests TEST_PREFIX "builder.")

    add_executable(
        lunar_terrain_sldem2015_acceptance_tests
        tests/builder/sldem2015_acceptance_tests.cpp
    )
    target_compile_features(lunar_terrain_sldem2015_acceptance_tests PRIVATE cxx_std_20)
    set_target_properties(
        lunar_terrain_sldem2015_acceptance_tests
        PROPERTIES CXX_EXTENSIONS OFF
    )
    target_compile_definitions(
        lunar_terrain_sldem2015_acceptance_tests
        PRIVATE
            LUNAR_TERRAIN_SLDEM_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/sldem2015_p1.toml"
    )
    target_link_libraries(
        lunar_terrain_sldem2015_acceptance_tests
        PRIVATE lunar_terrain_builder_lib Catch2::Catch2WithMain
    )
    lunar_terrain_set_warnings(lunar_terrain_sldem2015_acceptance_tests)
    lunar_terrain_enable_sanitizers(lunar_terrain_sldem2015_acceptance_tests)
    catch_discover_tests(
        lunar_terrain_sldem2015_acceptance_tests
        TEST_PREFIX "sldem2015."
        PROPERTIES LABELS sldem2015
    )

    add_test(NAME cli.version COMMAND lunar-terrain --version)
    set_tests_properties(cli.version PROPERTIES PASS_REGULAR_EXPRESSION "lunar-terrain 0.3.0")

    add_test(
        NAME provisioning.unit
        COMMAND Python3::Interpreter -m unittest discover
                -s "${CMAKE_CURRENT_SOURCE_DIR}/provisioning"
                -p "test_*.py" -v
    )
    add_test(
        NAME observer.unit
        COMMAND Python3::Interpreter -m unittest discover
                -s "${CMAKE_CURRENT_SOURCE_DIR}/tools"
                -p "test_*.py" -v
    )
    add_test(
        NAME cli.exit-codes
        COMMAND Python3::Interpreter
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_cli_exit_codes.py"
                --binary $<TARGET_FILE:lunar-terrain>
    )
    add_test(
        NAME cli.cancel-resume
        COMMAND Python3::Interpreter
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_cli_cancel_resume.py"
                --binary $<TARGET_FILE:lunar-terrain>
    )
    set_tests_properties(cli.cancel-resume PROPERTIES TIMEOUT 60)
    add_test(
        NAME qualification.m8-runner-unit
        COMMAND Python3::Interpreter -m unittest discover
                -s "${CMAKE_CURRENT_SOURCE_DIR}/qualification/m8"
                -p "test_*.py" -v
    )
    add_test(
        NAME qualification.m8-builder-lock-consistency
        COMMAND Python3::Interpreter
                "${CMAKE_CURRENT_SOURCE_DIR}/provisioning/verify_builder_locks.py"
                --lock "${CMAKE_CURRENT_SOURCE_DIR}/provisioning/provisioning.json"
                "${CMAKE_CURRENT_SOURCE_DIR}/qualification/m8/configs/profile_a.toml"
                "${CMAKE_CURRENT_SOURCE_DIR}/qualification/m8/configs/profile_b.toml"
                "${CMAKE_CURRENT_SOURCE_DIR}/qualification/m8/configs/profile_c.toml"
                "${CMAKE_CURRENT_SOURCE_DIR}/qualification/m8/configs/scale_probe.toml"
                "${CMAKE_CURRENT_SOURCE_DIR}/qualification/m8/configs/scale.toml"
    )

    set(lunar_terrain_synthetic_config "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/synthetic_p0.toml")
    set(lunar_terrain_synthetic_database "${CMAKE_CURRENT_SOURCE_DIR}/out/m2-synthetic/MoonSynthetic.ltdb")
    add_test(NAME cli.scan COMMAND lunar-terrain scan "${lunar_terrain_synthetic_config}" --json)
    set_tests_properties(cli.scan PROPERTIES PASS_REGULAR_EXPRESSION "\"dataset_id\"")
    add_test(NAME cli.plan COMMAND lunar-terrain plan "${lunar_terrain_synthetic_config}" --json)
    set_tests_properties(cli.plan PROPERTIES PASS_REGULAR_EXPRESSION "\"tile_count\":6")
    add_test(NAME cli.build COMMAND lunar-terrain build "${lunar_terrain_synthetic_config}" --json)
    set_tests_properties(
        cli.build
        PROPERTIES
            FIXTURES_SETUP m2_cli_database
            PASS_REGULAR_EXPRESSION "\"tile_count\":6"
    )
    add_test(
        NAME cli.validate
        COMMAND lunar-terrain validate "${lunar_terrain_synthetic_database}" --full --json
    )
    set_tests_properties(
        cli.validate
        PROPERTIES
            FIXTURES_REQUIRED m2_cli_database
            PASS_REGULAR_EXPRESSION "\"verified_seams\":12"
    )
    add_test(
        NAME cli.inspect
        COMMAND lunar-terrain inspect "${lunar_terrain_synthetic_database}"
                QSC/F4/L00/0000/0000 --json
    )
    set_tests_properties(
        cli.inspect
        PROPERTIES
            FIXTURES_REQUIRED m2_cli_database
            PASS_REGULAR_EXPRESSION "\"channel_count\":2"
    )
    add_test(
        NAME cli.diff
        COMMAND lunar-terrain diff "${lunar_terrain_synthetic_database}"
                "${lunar_terrain_synthetic_database}" --json
    )
    set_tests_properties(
        cli.diff
        PROPERTIES
            FIXTURES_REQUIRED m2_cli_database
            PASS_REGULAR_EXPRESSION "\"identical\":true"
    )
    add_test(
        NAME cli.export
        COMMAND lunar-terrain export "${lunar_terrain_synthetic_database}"
                QSC/F4/L00/0000/0000 --format raw-u16
                --output "${CMAKE_CURRENT_SOURCE_DIR}/out/m7-cli-elevation.u16"
    )
    set_tests_properties(cli.export PROPERTIES FIXTURES_REQUIRED m2_cli_database)
    add_test(
        NAME cli.benchmark
        COMMAND lunar-terrain benchmark "${lunar_terrain_synthetic_config}"
                --output "${CMAKE_CURRENT_SOURCE_DIR}/out/m7-cli-benchmark.json" --json
    )
    set_tests_properties(
        cli.benchmark
        PROPERTIES
            FIXTURES_REQUIRED m2_cli_database
            PASS_REGULAR_EXPRESSION "\"deterministic_rebuild\":true"
    )
endif()
