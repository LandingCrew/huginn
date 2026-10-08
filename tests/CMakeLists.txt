# =============================================================================
# huginn_core_tests -- host tests for src/core/ (see src/core/README.md)
# =============================================================================
# Builds and runs on the host, without the game: no CommonLib, no SKSE, and NOT
# the plugin's precompiled header (PCH.h is attached to the Huginn target only,
# PRIVATE). Compiling src/core/ here without it is what proves those headers
# stand on their own. The plugin compiles the same src/core/ files too (its
# GLOB_RECURSE over src/ picks them up), so the game layer calls this code.
#
# Test sources live here, outside src/, so the plugin's glob never compiles a
# test (or doctest's main) into Huginn.dll.
#
#   ctest -C Debug --test-dir build --output-on-failure
# =============================================================================

set(HUGINN_CORE_DIR "${PROJECT_SOURCE_DIR}/src/core")

# Guard the rule: src/core/ is pure. Fail the configure, not a late link, when
# a core file reaches for the game.
file(GLOB_RECURSE HUGINN_CORE_FILES CONFIGURE_DEPENDS
   "${HUGINN_CORE_DIR}/*.h" "${HUGINN_CORE_DIR}/*.hpp" "${HUGINN_CORE_DIR}/*.cpp")
foreach(coreFile IN LISTS HUGINN_CORE_FILES)
   file(STRINGS "${coreFile}" offending
      REGEX "#[ \t]*include[ \t]*[<\"](RE/|REL/|SKSE/|PCH\\.h|SimpleIni|spdlog/)|[^A-Za-z_](RE|REL|SKSE)::")
   if(offending)
      message(FATAL_ERROR "src/core/ must stay pure (see src/core/README.md); "
         "${coreFile} has: ${offending}")
   endif()
endforeach()

file(GLOB HUGINN_CORE_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/core/*.cpp")
file(GLOB_RECURSE HUGINN_CORE_SOURCES CONFIGURE_DEPENDS "${HUGINN_CORE_DIR}/*.cpp")

find_package(doctest CONFIG REQUIRED)

add_executable(huginn_core_tests ${HUGINN_CORE_TEST_SOURCES} ${HUGINN_CORE_SOURCES})
target_compile_features(huginn_core_tests PRIVATE cxx_std_23)
# Only src/ is on the include path, and core headers are included as
# "core/X.h". A test that includes a game header fails to compile here (no
# PCH, no CommonLib), which is the point.
target_include_directories(huginn_core_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(huginn_core_tests PRIVATE doctest::doctest)
if(MSVC)
   target_compile_options(huginn_core_tests PRIVATE /utf-8 /permissive- /Zc:preprocessor)
endif()

add_test(NAME huginn_core_tests COMMAND huginn_core_tests)
