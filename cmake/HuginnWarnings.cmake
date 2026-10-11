# =============================================================================
# huginn_enable_warnings(<target>) -- the warning level for first-party targets
# =============================================================================
# The preset's global CMAKE_CXX_FLAGS carry no /W level, so each target sets its
# own. First-party targets (Huginn, huginn_core_tests, huginn_effect_report) get
# /W4 here; third-party code keeps the level it had before (CommonLibSSE and
# Tracy are pinned to /W0 where they are added; vcpkg ports are built by vcpkg
# with its own flags and never see ours).
#
# Third-party headers stay quiet in our TUs:
#   /external:anglebrackets  every #include <...> is an external header
#   /external:W0             external headers emit no warnings
# and CommonLibSSE is added with add_subdirectory(... SYSTEM), so its include
# directory reaches us as /external:I even where a header is pulled in with
# quotes. Imported targets (vcpkg: spdlog, fmt, imgui, freetype, doctest) are
# SYSTEM by default.
#
# /WX: warnings are errors on our targets, so the build stays at 0 warnings.
# Configure with -DHUGINN_WARNINGS_AS_ERRORS=OFF to turn that off locally.
# =============================================================================

option(HUGINN_WARNINGS_AS_ERRORS "Treat compiler warnings as errors on Huginn's own targets" ON)

function(huginn_enable_warnings target)
   if(MSVC)
      target_compile_options("${target}" PRIVATE
         /W4
         /external:anglebrackets
         /external:W0
         $<$<BOOL:${HUGINN_WARNINGS_AS_ERRORS}>:/WX>)
   else()
      target_compile_options("${target}" PRIVATE
         -Wall -Wextra
         $<$<BOOL:${HUGINN_WARNINGS_AS_ERRORS}>:-Werror>)
   endif()
endfunction()
