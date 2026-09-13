cmake_minimum_required(VERSION 3.20)
project(iso-recorder VERSION 0.2.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(OBS_SRC "" CACHE PATH "Path to an obs-studio source checkout (headers)")
set(OBS_APP "" CACHE PATH "Path to the installed OBS.app (macOS)")

find_package(Qt6 REQUIRED COMPONENTS Widgets)
set(CMAKE_AUTOMOC ON)

if(NOT OBS_SRC)
  message(FATAL_ERROR "Pass -DOBS_SRC=<obs-studio checkout>")
endif()

# libobs's util/sse-intrin.h includes <emmintrin.h> under MSVC on x86/x64 and
# <simde/x86/sse2.h> on the other targets this project builds (Apple, ARM). Demanding
# SIMDe where it is not included would block an MSVC x64 configure for no reason.
set(OBS_NEEDS_SIMDE TRUE)
if(MSVC AND (CMAKE_SIZEOF_VOID_P EQUAL 4 OR
             (CMAKE_SIZEOF_VOID_P EQUAL 8 AND
              NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID MATCHES "ARM")))
  set(OBS_NEEDS_SIMDE FALSE)
endif()
if(OBS_NEEDS_SIMDE AND NOT EXISTS "${OBS_SRC}/deps/simde/simde/x86/sse2.h")
  message(FATAL_ERROR
    "SIMDe headers not found at ${OBS_SRC}/deps/simde. Fetch them, e.g.\n"
    "  git clone --depth 1 --branch v0.8.2 https://github.com/simd-everywhere/simde.git \"${OBS_SRC}/deps/simde\"")
endif()

# libobs's obs-config.h includes obsconfig.h, which OBS generates during its own
# build and a headers-only checkout does not contain.
set(OBS_CONFIG_DIR "${CMAKE_CURRENT_BINARY_DIR}/obsconfig")
file(MAKE_DIRECTORY "${OBS_CONFIG_DIR}")
file(WRITE "${OBS_CONFIG_DIR}/obsconfig.h"
  "#pragma once\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n")

set(OBS_INCLUDES
  "${OBS_SRC}/libobs"
  "${OBS_SRC}/frontend/api"
  "${OBS_CONFIG_DIR}"
)
if(OBS_NEEDS_SIMDE)
  list(APPEND OBS_INCLUDES "${OBS_SRC}/deps/simde")
endif()
include_directories(SYSTEM ${OBS_INCLUDES})
add_compile_definitions(OBS_SWIG=1)

if(APPLE)
  if(NOT OBS_APP)
    set(OBS_APP "/Applications/OBS.app")
  endif()
  set(OBS_FW "${OBS_APP}/Contents/Frameworks")
  # The app ships libobs.framework and a bare obs-frontend-api.dylib. find_library
  # resolves the framework but not the no-prefix dylib, so both concrete paths are used.
  # libobs is the binary inside the framework; linking the .framework directory as a
  # library makes CMake add OBS.app's header-less Qt frameworks to the compile search
  # path, where they shadow Homebrew's and break every <QtWidgets/...> include.
  set(OBS_LIB "${OBS_FW}/libobs.framework/libobs")
  set(OBS_FRONTEND_LIB "${OBS_FW}/obs-frontend-api.dylib")
  if(NOT EXISTS "${OBS_LIB}" OR NOT EXISTS "${OBS_FRONTEND_LIB}")
    message(FATAL_ERROR "OBS libraries not found under ${OBS_FW}")
  endif()
elseif(WIN32)
  find_library(OBS_LIB obs REQUIRED)
  find_library(OBS_FRONTEND_LIB obs-frontend-api REQUIRED)
else()
  message(FATAL_ERROR "macOS and Windows only")
endif()

# Later tasks append their sources to this target and add their test targets below.
add_library(iso-recorder MODULE
  src/plugin-main.cpp
  src/session-writer.cpp
  src/source-recorder.cpp
  src/iso-session.cpp
  src/iso-dock.cpp
  src/iso-settings.cpp
  src/wav-writer.cpp
)
target_link_libraries(iso-recorder PRIVATE Qt6::Widgets)
# So the log can say which build is running: telling a stale install from a new
# one is otherwise guesswork once the dock is on screen.
target_compile_definitions(iso-recorder PRIVATE ISO_VERSION="${PROJECT_VERSION}")
if(APPLE)
  # The OBS paths are raw link options, not target_link_libraries entries: a linked path
  # containing ".framework" makes CMake add OBS.app's header-less Qt frameworks to the
  # compile include path, where they shadow Homebrew's. The runtime link is unchanged.
  target_link_options(iso-recorder PRIVATE "${OBS_LIB}" "${OBS_FRONTEND_LIB}")
else()
  target_link_libraries(iso-recorder PRIVATE ${OBS_LIB} ${OBS_FRONTEND_LIB})
endif()

set_target_properties(iso-recorder PROPERTIES PREFIX "" OUTPUT_NAME iso-recorder)
if(APPLE)
  # A MODULE library ignores MACOSX_BUNDLE; OBS's own plugins use BUNDLE + BUNDLE_EXTENSION.
  set_target_properties(iso-recorder PROPERTIES
    BUNDLE TRUE BUNDLE_EXTENSION plugin
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/mac/Info.plist.in")

  # Runtime Qt must come from OBS, not Homebrew.
  #
  # The plugin compiles and moc's against Homebrew qtbase (6.11.2), but OBS.app ships its
  # own Qt (6.11.1) and loads it first. The plugin's recorded absolute Homebrew framework
  # paths share their @rpath install names with OBS's Qt, so dyld binds the plugin's Qt
  # calls to the already-loaded 6.11.1 frameworks and fails on a versioned private symbol
  # they lack (e.g. QtPrivate_6_11_2), refusing to load the module. The two builds are
  # patch-compatible on Qt's public 6.11 ABI, and the plugin references no versioned Qt
  # private symbols, so binding to OBS's Qt at runtime is safe. Rewrite the three framework
  # load commands the plugin actually links into OBS's @rpath form, and add the rpath that
  # resolves @rpath to OBS.app/Contents/Frameworks. install_name_tool invalidates the
  # linker's ad-hoc signature, so re-sign after the rewrites.
  get_target_property(_qt_core Qt6::Core IMPORTED_LOCATION)
  string(REPLACE "QtCore.framework/Versions/A/QtCore" "" _qt_lib_dir "${_qt_core}")
  find_program(CODESIGN codesign REQUIRED)
  find_program(INSTALL_NAME_TOOL install_name_tool REQUIRED)
  set(_qt_rewrite_commands "")
  foreach(_fw QtWidgets QtGui QtCore)
    list(APPEND _qt_rewrite_commands
      COMMAND "${INSTALL_NAME_TOOL}" -change
        "${_qt_lib_dir}${_fw}.framework/Versions/A/${_fw}"
        "@rpath/${_fw}.framework/Versions/A/${_fw}"
        "$<TARGET_FILE:iso-recorder>")
  endforeach()
  add_custom_command(TARGET iso-recorder POST_BUILD
    ${_qt_rewrite_commands}
    COMMAND "${CODESIGN}" --force --sign - "$<TARGET_FILE:iso-recorder>"
    COMMENT "Binding iso-recorder to OBS's bundled Qt frameworks and re-signing")
  target_link_options(iso-recorder PRIVATE "-Wl,-rpath,@executable_path/../Frameworks")
endif()

enable_testing()
add_executable(test_session_writer
  tests/test_session_writer.cpp
  src/session-writer.cpp
)
target_include_directories(test_session_writer PRIVATE src)
add_test(NAME session_writer COMMAND test_session_writer)

add_executable(test_wav_writer
  tests/test_wav_writer.cpp
  src/wav-writer.cpp
)
target_include_directories(test_wav_writer PRIVATE src)
add_test(NAME wav_writer COMMAND test_wav_writer)
