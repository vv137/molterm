cmake_minimum_required(VERSION 3.16)
project(molterm VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Dependencies ─────────────────────────────────────────────────────────────

include(FetchContent)

# gemmi — mmCIF/PDB parser (header-only)
FetchContent_Declare(
    gemmi
    GIT_REPOSITORY https://github.com/project-gemmi/gemmi.git
    GIT_TAG        v0.7.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(gemmi)

# USalign — structural alignment
FetchContent_Declare(
    usalign
    GIT_REPOSITORY https://github.com/pylelab/USalign.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(usalign)
if(NOT usalign_POPULATED)
    FetchContent_Populate(usalign)
endif()

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

set(MOLTERM_SOURCES
    src/main.cpp
    # Core
    src/core/MolObject.cpp
    src/core/ObjectStore.cpp
    src/core/Selection.cpp
    # IO
    src/io/CifLoader.cpp
    src/io/Aligner.cpp
    # TUI
    src/tui/Screen.cpp
    src/tui/Window.cpp
    src/tui/Layout.cpp
    src/tui/StatusBar.cpp
    src/tui/CommandLine.cpp
    src/tui/TabBar.cpp
    src/tui/ObjectPanel.cpp
    # Render
    src/render/Camera.cpp
    src/render/ColorMapper.cpp
    src/render/Canvas.cpp
    src/render/AsciiCanvas.cpp
    src/render/BrailleCanvas.cpp
    src/render/BlockCanvas.cpp
    src/render/SixelCanvas.cpp
    src/render/AsciiRenderer.cpp
    # Repr
    src/repr/WireframeRepr.cpp
    src/repr/BallStickRepr.cpp
    src/repr/BackboneRepr.cpp
    src/repr/SpacefillRepr.cpp
    src/repr/CartoonRepr.cpp
    # Input
    src/input/Keymap.cpp
    src/input/KeymapManager.cpp
    src/input/InputHandler.cpp
    # Cmd
    src/cmd/CommandParser.cpp
    src/cmd/CommandRegistry.cpp
    src/cmd/UndoStack.cpp
    # App
    src/app/Tab.cpp
    src/app/TabManager.cpp
    src/app/Application.cpp
)

# ── Target ───────────────────────────────────────────────────────────────────

add_executable(molterm ${MOLTERM_SOURCES})

target_include_directories(molterm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    $<$<NOT:$<BOOL:${NCURSESW_FOUND}>>:${CURSES_INCLUDE_DIR}>
)

target_link_libraries(molterm PRIVATE
    gemmi::gemmi_cpp
    $<IF:$<BOOL:${NCURSESW_FOUND}>,PkgConfig::NCURSESW,${CURSES_LIBRARIES}>
)

target_compile_options(molterm PRIVATE
    -Wall -Wextra -Wpedantic
)

target_compile_definitions(molterm PRIVATE
    USALIGN_PATH="${CMAKE_BINARY_DIR}/USalign"
    NCURSES_WIDECHAR=1
)

# molterm depends on USalign binary being built
add_dependencies(molterm USalign)

# ── Install ──────────────────────────────────────────────────────────────────

install(TARGETS molterm DESTINATION bin)
