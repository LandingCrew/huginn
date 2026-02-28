set(ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")

set(SOURCE_DIR "${ROOT_DIR}/src")
file(GLOB_RECURSE SOURCE_FILES "${SOURCE_DIR}/*.cpp" "${SOURCE_DIR}/*.h" "${SOURCE_DIR}/*.hpp")

# Organize source files for Visual Studio (preserve directory structure)
foreach(fileItem ${SOURCE_FILES})
    # Get the directory of the source file
    get_filename_component(PARENT_DIR "${fileItem}" DIRECTORY)
    # Remove common directory prefix to make the group
    string(REPLACE "${CMAKE_CURRENT_SOURCE_DIR}" "" GROUP "${PARENT_DIR}")
    # Make sure we are using windows slashes
    string(REPLACE "/" "\\" GROUP "${GROUP}")
    # Group into "Source Files" and "Header Files"
    set(GROUP "${GROUP}")
    source_group("${GROUP}" FILES "${fileItem}")
endforeach()

source_group(TREE "${ROOT_DIR}" FILES ${SOURCE_FILES})

set(VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/src/Plugin.h")
configure_file(
   "${ROOT_DIR}/cmake/Plugin.h.in"
   "${VERSION_HEADER}"
   @ONLY
)

source_group("src" FILES "${VERSION_HEADER}")

configure_file(
   "${ROOT_DIR}/cmake/version.rc.in"
   "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
   @ONLY
)

add_library(
   "${PROJECT_NAME}"
   SHARED
   ${SOURCE_FILES}
   "${VERSION_HEADER}"
   "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
   "${ROOT_DIR}/.clang-format"
   "${ROOT_DIR}/.editorconfig"
)

target_compile_features(
   "${PROJECT_NAME}"
   PRIVATE
      cxx_std_23
)

if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
   # Remove default /Ob2 to avoid warning when we set /Ob3
   string(REPLACE "/Ob2" "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")

   target_compile_options(
      "${PROJECT_NAME}"
      PRIVATE
      "/sdl"   # Enable Additional Security Checks
      "/utf-8"   # Set Source and Executable character sets to UTF-8
      "/Zi"   # Debug Information Format

      "/permissive-"   # Standards conformance
      "/Zc:preprocessor"   # Enable preprocessor conformance mode

      "/wd4200" # nonstandard extension used : zero-sized array in struct/union

      "$<$<CONFIG:DEBUG>:>"
      "$<$<CONFIG:RELEASE>:/Zc:inline;/JMC-;/Ob3>"
   )

   target_link_options(
      "${PROJECT_NAME}"
      PRIVATE
      "$<$<CONFIG:DEBUG>:/INCREMENTAL;/OPT:NOREF;/OPT:NOICF>"
      "$<$<CONFIG:RELEASE>:/INCREMENTAL:NO;/OPT:REF;/OPT:ICF;/DEBUG:FULL>"
   )
endif()

target_include_directories(
   "${PROJECT_NAME}"
   PRIVATE
      "${CMAKE_CURRENT_BINARY_DIR}/src"
      "${SOURCE_DIR}"
)

target_compile_definitions(
   "${PROJECT_NAME}"
   PRIVATE
      Huginn_EXPORTS
      "$<$<CONFIG:Debug>:_DEBUG>"
)

if(NOT TARGET CommonLibSSE::CommonLibSSE)
   add_subdirectory("$ENV{CommonLibSSEPath_NG}" CommonLibSSE EXCLUDE_FROM_ALL)
endif()

find_package(imgui REQUIRED)

find_package(Freetype REQUIRED)

target_link_libraries(
   "${PROJECT_NAME}"
   PRIVATE
      CommonLibSSE::CommonLibSSE
      imgui::imgui
      Freetype::Freetype
)

# Tracy profiler integration
if(Huginn_TRACY)
   target_link_libraries("${PROJECT_NAME}" PRIVATE TracyClient)
   target_compile_definitions("${PROJECT_NAME}" PRIVATE Huginn_TRACY_ENABLED)
endif()

target_precompile_headers(
   "${PROJECT_NAME}"
   PRIVATE
      "${SOURCE_DIR}/PCH.h"
)

install(
   FILES
      "$<TARGET_FILE:${PROJECT_NAME}>"
   DESTINATION "SKSE/Plugins"
   COMPONENT "main"
)

install(
   FILES
      "$<TARGET_PDB_FILE:${PROJECT_NAME}>"
   DESTINATION "/"
   COMPONENT "pdbs"
)

# Install configuration files
install(
   FILES
      "${ROOT_DIR}/configs/Huginn.ini"
   DESTINATION "SKSE/Plugins"
   COMPONENT "main"
)

# Install Scaleform widget
install(
   FILES
      "${SOURCE_DIR}/swf/Intuition.swf"
   DESTINATION "Interface/Huginn"
   COMPONENT "main"
)

# Install dMenu integration (optional component)
install(
   FILES
      "${ROOT_DIR}/Data/SKSE/Plugins/dmenu/customSettings/Huginn.json"
   DESTINATION "SKSE/Plugins/dmenu/customSettings"
   COMPONENT "dmenu"
)

if("${COPY_OUTPUT}")
   add_custom_command(
      TARGET "${PROJECT_NAME}"
      POST_BUILD
      # Copy DLL and PDB
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${CompiledPluginsPath}/SKSE/Plugins/"
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_PDB_FILE:${PROJECT_NAME}>" "${CompiledPluginsPath}/SKSE/Plugins/"

      # Copy configuration
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${ROOT_DIR}/configs/Huginn.ini" "${CompiledPluginsPath}/SKSE/Plugins/Huginn.ini"

      # Copy Scaleform widget
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${CompiledPluginsPath}/Interface/Huginn"
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SOURCE_DIR}/swf/Intuition.swf" "${CompiledPluginsPath}/Interface/Huginn/Intuition.swf"

      # Copy dMenu integration
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${CompiledPluginsPath}/SKSE/Plugins/dmenu/customSettings"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${CompiledPluginsPath}/SKSE/Plugins/dmenu/customSettings/ini"
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${ROOT_DIR}/Data/SKSE/Plugins/dmenu/customSettings/Huginn.json" "${CompiledPluginsPath}/SKSE/Plugins/dmenu/customSettings/Huginn.json"

      VERBATIM
   )
endif()
