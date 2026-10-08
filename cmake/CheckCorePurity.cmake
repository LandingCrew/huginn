# =============================================================================
# CheckCorePurity.cmake -- src/core/ must not reach for the game
# =============================================================================
# Run as a script:  cmake -DCORE_DIR=<src/core> -DSRC_DIR=<src> -P CheckCorePurity.cmake
# Called at configure time and, through a custom command, on every build of
# huginn_core_tests and Huginn whose core files changed (tests/CMakeLists.txt).
#
# Rules, for EVERY file under CORE_DIR whatever its extension (a quoted
# include can pull in core/foo.inc), except Markdown (*.md, documentation that
# has to name what it forbids):
#   1. A quoted include must resolve to a file under CORE_DIR: first against
#      the including file's folder, then against SRC_DIR (the plugin's include
#      root, so "core/X.h" works). "Globals.h", "state/GameState.h",
#      "../PCH.h" and anything that resolves nowhere are rejected. It must
#      also name a source file (.h .hpp .hxx .inl .ipp .inc), so the
#      unscanned Markdown -- or a .txt, .cpp.in, ... -- can never be pulled
#      into a translation unit.
#   2. An angle include must be a C++ standard library header (the list
#      below). RE/, REL/, REX/, SKSE/, spdlog/, SimpleIni.h, Windows.h ... are
#      rejected because they are not on it.
#   3. Include paths use '/': a backslash is rejected.
#   4. No game namespaces: RE, REL, REX, SKSE, logger or spdlog followed by
#      '::' (with or without spaces, at any column); no `using namespace` of,
#      namespace alias to, or reopening of RE/REL/REX/SKSE (a leading '::'
#      and whitespace allowed: `using namespace ::RE;`, `namespace G = ::SKSE;`,
#      `namespace RE {`).
# The scan is textual: a comment that names RE:: or a game header trips it
# too. Write "the game's types" in comments instead. It is not a parser; the
# known ways around it are listed in src/core/README.md, and the real check is
# that every core header compiles on its own without the PCH or CommonLib.
# =============================================================================

if(NOT CORE_DIR OR NOT SRC_DIR)
   message(FATAL_ERROR "CheckCorePurity: pass -DCORE_DIR=... -DSRC_DIR=...")
endif()
get_filename_component(CORE_DIR "${CORE_DIR}" ABSOLUTE)
get_filename_component(SRC_DIR "${SRC_DIR}" ABSOLUTE)

set(STD_HEADERS
   algorithm any array atomic barrier bit bitset cassert cctype cerrno cfenv
   cfloat charconv chrono cinttypes climits clocale cmath compare complex
   concepts condition_variable coroutine csetjmp csignal cstdarg cstddef
   cstdint cstdio cstdlib cstring ctime cuchar cwchar cwctype deque exception
   execution expected filesystem format forward_list fstream functional
   future initializer_list iomanip ios iosfwd iostream istream iterator latch
   limits list locale map mdspan memory memory_resource mutex new numbers
   numeric optional ostream print queue random ranges ratio regex
   scoped_allocator semaphore set shared_mutex source_location span
   spanstream sstream stack stacktrace stdexcept stdfloat stop_token
   streambuf string string_view syncstream system_error thread tuple
   type_traits typeindex typeinfo unordered_map unordered_set utility
   valarray variant vector version)

file(GLOB_RECURSE core_files "${CORE_DIR}/*")
list(FILTER core_files EXCLUDE REGEX "\\.[mM][dD]$")

set(violations "")
# A function, not a macro: macro arguments are substituted as text, and an
# include line's own quotes would then break the string.
function(reject file what)
   file(RELATIVE_PATH _rel "${SRC_DIR}" "${file}")
   set(violations "${violations}\n  src/${_rel}: ${what}" PARENT_SCOPE)
endfunction()

function(_under_core path out)
   string(LENGTH "${CORE_DIR}/" _n)
   string(SUBSTRING "${path}/" 0 ${_n} _head)
   if(_head STREQUAL "${CORE_DIR}/")
      set(${out} TRUE PARENT_SCOPE)
   else()
      set(${out} FALSE PARENT_SCOPE)
   endif()
endfunction()

foreach(f IN LISTS core_files)
   file(READ "${f}" text)
   get_filename_component(fdir "${f}" DIRECTORY)

   # --- includes ------------------------------------------------------------
   string(REGEX MATCHALL "#[ \t]*include[ \t]*(<[^>\n]*>|\"[^\"\n]*\")" incs "${text}")
   foreach(inc IN LISTS incs)
      string(REGEX REPLACE "^#[ \t]*include[ \t]*" "" spec "${inc}")
      string(SUBSTRING "${spec}" 0 1 delim)
      string(LENGTH "${spec}" len)
      math(EXPR inner_len "${len} - 2")
      string(SUBSTRING "${spec}" 1 ${inner_len} path)
      if(path MATCHES "\\\\")
         reject("${f}" "include with a backslash: ${inc}")
         continue()
      endif()
      if(delim STREQUAL "<")
         if(NOT path IN_LIST STD_HEADERS)
            reject("${f}" "angle include not in the standard-library allow-list: ${inc}")
         endif()
      else()
         # Only source extensions may be included: the Markdown exemption
         # (and any other unscanned file) must not be reachable by #include.
         if(NOT path MATCHES "\\.(h|hpp|hxx|inl|ipp|inc)$")
            reject("${f}" "quoted include of a non-source file (allowed: .h .hpp .hxx .inl .ipp .inc): ${inc}")
            continue()
         endif()
         get_filename_component(local "${path}" ABSOLUTE BASE_DIR "${fdir}")
         get_filename_component(rooted "${path}" ABSOLUTE BASE_DIR "${SRC_DIR}")
         if(EXISTS "${local}" AND NOT IS_DIRECTORY "${local}")
            set(target "${local}")
         elseif(EXISTS "${rooted}" AND NOT IS_DIRECTORY "${rooted}")
            set(target "${rooted}")
         else()
            reject("${f}" "quoted include that resolves to no file: ${inc}")
            continue()
         endif()
         _under_core("${target}" inside)
         if(NOT inside)
            reject("${f}" "quoted include outside src/core: ${inc}")
         endif()
      endif()
   endforeach()

   # --- game namespaces -----------------------------------------------------
   string(REGEX MATCHALL "(^|[^A-Za-z0-9_])(RE|REL|REX|SKSE|logger|spdlog)[ \t\r\n]*::" hits "${text}")
   foreach(h IN LISTS hits)
      string(STRIP "${h}" h)
      if(h STREQUAL "")
         continue()   # a match that ended in ";" splits into an empty list item
      endif()
      reject("${f}" "game namespace: ${h}")
   endforeach()
   string(REGEX MATCHALL "using[ \t\r\n]+namespace[ \t\r\n]+(::[ \t\r\n]*)?(RE|REL|REX|SKSE)([^A-Za-z0-9_]|$)" hits "${text}")
   foreach(h IN LISTS hits)
      string(STRIP "${h}" h)
      if(h STREQUAL "")
         continue()   # a match that ended in ";" splits into an empty list item
      endif()
      reject("${f}" "${h}")
   endforeach()
   string(REGEX MATCHALL "namespace[ \t\r\n]+[A-Za-z_][A-Za-z0-9_]*[ \t\r\n]*=[ \t\r\n]*(::[ \t\r\n]*)?(RE|REL|REX|SKSE)([^A-Za-z0-9_]|$)" hits "${text}")
   foreach(h IN LISTS hits)
      string(STRIP "${h}" h)
      if(h STREQUAL "")
         continue()   # a match that ended in ";" splits into an empty list item
      endif()
      reject("${f}" "namespace alias to the game: ${h}")
   endforeach()
   # Reopening a game namespace: namespace RE {, namespace RE::detail {
   string(REGEX MATCHALL "namespace[ \t\r\n]+(::[ \t\r\n]*)?(RE|REL|REX|SKSE)[ \t\r\n]*({|::)" hits "${text}")
   foreach(h IN LISTS hits)
      string(STRIP "${h}" h)
      if(h STREQUAL "")
         continue()
      endif()
      reject("${f}" "reopens a game namespace: ${h}")
   endforeach()
endforeach()

if(violations)
   message(FATAL_ERROR "src/core/ must stay pure (see src/core/README.md):${violations}")
endif()
list(LENGTH core_files n)
message(STATUS "src/core/ purity: ${n} file(s) clean")
