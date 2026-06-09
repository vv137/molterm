cmake_minimum_required(VERSION 3.16)
project(molterm VERSION 0.47.8 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Dependencies ─────────────────────────────────────────────────────────────

include(FetchContent)

# gemmi — mmCIF/PDB parser (static link for portable binaries)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_GEMMI_PROGRAM OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    gemmi
    GIT_REPOSITORY https://github.com/project-gemmi/gemmi.git
    GIT_TAG        v0.7.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(gemmi)
# NOTE: gemmi unconditionally registers a `cpptest` CTest case bound to an
# EXCLUDE_FROM_ALL executable (never built). It can't be unregistered from this
# (parent) scope in CMake 3.16, so molterm's own tests carry a `molterm` label
# and CI/`ctest` select them with `-L molterm` (see tests/CMakeLists.txt).

# toml++ — TOML config parser (header-only)
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(tomlplusplus)

# USalign — structural alignment
FetchContent_Declare(
    usalign
    GIT_REPOSITORY https://github.com/pylelab/USalign.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(usalign)

# Build USalign binary from single source
add_executable(USalign ${usalign_SOURCE_DIR}/USalign.cpp)
target_compile_options(USalign PRIVATE -O3 -ffast-math)
set_target_properties(USalign PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# ncurses — prefer wide-character build (ncursesw) for Unicode support.
# On macOS the system curses already includes wide-char support.
# On Linux we must link against ncursesw explicitly and include its
# headers from /usr/include/ncursesw/ so the wide-char API is declared.
set(NCURSESW_FOUND FALSE)
if(NOT APPLE)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(NCURSESW QUIET IMPORTED_TARGET ncursesw)
    endif()
endif()
if(NOT NCURSESW_FOUND)
    find_package(Curses REQUIRED)
endif()

# ── Sources ──────────────────────────────────────────────────────────────────

# Everything except the entry point lives in the molterm_core library so the
# test suite (tests/CMakeLists.txt) can link the same objects the app uses
# instead of re-compiling individual translation units by hand.
set(MOLTERM_CORE_SOURCES
    # Core
    src/core/Geometry.cpp
    src/core/MolObject.cpp
    src/core/ObjectStore.cpp
    src/core/Selection.cpp
    src/core/Logger.cpp
    src/core/BondTable.cpp
    src/core/DSSP.cpp
    src/core/SASA.cpp
    # IO
    src/io/CifLoader.cpp
    src/io/CcdCache.cpp
    src/io/PdbWriter.cpp
    src/io/Aligner.cpp
    # Analysis
    src/analysis/ContactMap.cpp
    # TUI
    src/tui/Screen.cpp
    src/tui/Window.cpp
    src/tui/Constraint.cpp
    src/tui/Layout.cpp
    src/tui/StatusBar.cpp
    src/tui/CommandLine.cpp
    src/tui/TabBar.cpp
    src/tui/ObjectPanel.cpp
    src/tui/SeqBar.cpp
    src/tui/DensityMap.cpp
    src/tui/ContactMapPanel.cpp
    # Render
    src/render/Camera.cpp
    src/render/ColorMapper.cpp
    src/render/Canvas.cpp
    src/render/AsciiCanvas.cpp
    src/render/BrailleCanvas.cpp
    src/render/BlockCanvas.cpp
    src/render/PixelCanvas.cpp
    src/render/stb_truetype_impl.cpp
    src/render/SixelEncoder.cpp
    src/render/KittyEncoder.cpp
    src/render/ITermEncoder.cpp
    src/render/ProtocolPicker.cpp
    src/render/AsciiRenderer.cpp
    # Repr
    src/repr/WireframeRepr.cpp
    src/repr/BallStickRepr.cpp
    src/repr/Representation.cpp
    src/repr/BackboneRepr.cpp
    src/repr/SpacefillRepr.cpp
    src/repr/CartoonRepr.cpp
    src/repr/RibbonRepr.cpp
    src/repr/InterfaceRepr.cpp
    src/repr/SurfaceRepr.cpp
    # Input
    src/input/Keymap.cpp
    src/input/KeymapManager.cpp
    src/input/InputHandler.cpp
    # Cmd
    src/cmd/CommandParser.cpp
    src/cmd/CommandRegistry.cpp
    src/cmd/CommandHelpers.cpp
    src/cmd/ExprContext.cpp
    src/cmd/Register.cpp
    src/cmd/RegisterExpr.cpp
    src/cmd/UndoStack.cpp
    # Cmd/Commands — per-area command registration (decomposed registerCommands)
    src/cmd/commands/Session.cpp
    src/cmd/commands/Files.cpp
    # App
    src/app/Tab.cpp
    src/app/TabManager.cpp
    src/app/Application.cpp
    src/app/CommandScope.cpp
    src/app/PathPatterns.cpp
    # Config
    src/config/ConfigParser.cpp
    # Export
    src/io/SessionExporter.cpp
    src/io/SessionSaver.cpp
)

# ── Embedded font ────────────────────────────────────────────────────────────

set(FONT_INPUT "${CMAKE_CURRENT_SOURCE_DIR}/resources/SpaceMono-Regular.ttf")
set(FONT_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/font_data.h")
add_custom_command(
    OUTPUT "${FONT_HEADER}"
    COMMAND ${CMAKE_COMMAND} -DINPUT=${FONT_INPUT} -DOUTPUT=${FONT_HEADER}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/resources/embed_font.cmake"
    DEPENDS "${FONT_INPUT}" "${CMAKE_CURRENT_SOURCE_DIR}/resources/embed_font.cmake"
    COMMENT "Embedding font as C header"
)
add_custom_target(generate_font DEPENDS "${FONT_HEADER}")

# Get version string: use git tag if on one, otherwise dev+hash
execute_process(
    COMMAND git describe --tags --exact-match
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GIT_TAG_RESULT
)
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(GIT_TAG_RESULT EQUAL 0 AND GIT_TAG)
    set(MOLTERM_VERSION_STRING "${GIT_TAG}")
elseif(GIT_HASH)
    set(MOLTERM_VERSION_STRING "dev+${GIT_HASH}")
else()
    set(MOLTERM_VERSION_STRING "dev")
endif()

find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

# ── Core library ─────────────────────────────────────────────────────────────
# All app/interpreter logic, minus the entry point. The executable and the
# test suite both link this, so tests exercise the exact objects that ship.

add_library(molterm_core STATIC ${MOLTERM_CORE_SOURCES})
add_dependencies(molterm_core generate_font USalign)

# PUBLIC so consumers (molterm, tests) inherit the include paths, link deps,
# and compile definitions transitively.
target_include_directories(molterm_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    $<$<NOT:$<BOOL:${NCURSESW_FOUND}>>:${CURSES_INCLUDE_DIR}>
)
target_link_libraries(molterm_core PUBLIC
    gemmi::gemmi_cpp
    tomlplusplus::tomlplusplus
    ZLIB::ZLIB
    Threads::Threads
    $<IF:$<BOOL:${NCURSESW_FOUND}>,PkgConfig::NCURSESW,${CURSES_LIBRARIES}>
)
target_compile_definitions(molterm_core PUBLIC
    USALIGN_PATH="${CMAKE_BINARY_DIR}/USalign"
    NCURSES_WIDECHAR=1
    MOLTERM_VERSION="${MOLTERM_VERSION_STRING}"
    MOLTERM_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_compile_options(molterm_core PRIVATE -Wall -Wextra -Wpedantic)

# ── Executable ───────────────────────────────────────────────────────────────

add_executable(molterm src/main.cpp)
target_link_libraries(molterm PRIVATE molterm_core)
target_compile_options(molterm PRIVATE -Wall -Wextra -Wpedantic)

# ── Tests (CTest) ────────────────────────────────────────────────────────────

option(MOLTERM_BUILD_TESTS "Build the unit test suite" ON)
if(MOLTERM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ── Install ──────────────────────────────────────────────────────────────────

install(TARGETS molterm DESTINATION bin)
# Ship the analysis recipe library — discoverable from scripts via
# `:run @lib/<name>`. Per-user overrides at ~/.molterm/lib/ and the
# $MOLTERM_LIB_DIR env var take precedence at lookup time (issue #56).
install(DIRECTORY lib/ DESTINATION share/molterm/lib
        FILES_MATCHING PATTERN "*.mt")
