cmake_minimum_required(VERSION 3.20)
project(WG3DRecomp VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
    cmake_policy(SET CMP0135 NEW)  # DOWNLOAD_EXTRACT_TIMESTAMP
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# ---------------------------------------------------------------------------------------------
# Dependencies (Phase 3). Same setup as Zelda64Recomp / Quest64-Recomp.
#   lib/N64ModernRuntime: sonicdcer/controller_pak_super_rebase @ 6021a7d (local branch `wg3d`):
#     upstream 589bbf0 + the Controller Pak implementation (N64ModernRuntime PR #139 lineage),
#     which provides osPfsInit/osPfsInitPak etc. Pins N64Recomp 81213c1.
#   lib/rt64: renderer (static, SDL window for Vulkan, CPU-side HLSL helpers).
#   SDL2: official VC development package via FetchContent.
# ---------------------------------------------------------------------------------------------
set(RT64_STATIC TRUE)
set(RT64_SDL_WINDOW_VULKAN TRUE)
add_compile_definitions(HLSL_CPU)

include(FetchContent)
set(SDL2_VERSION "2.30.3" CACHE STRING "SDL2 version to fetch")
FetchContent_Declare(sdl2
    URL https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-devel-${SDL2_VERSION}-VC.zip)
FetchContent_MakeAvailable(sdl2)
include_directories(${sdl2_SOURCE_DIR}/include)
link_directories(${sdl2_SOURCE_DIR}/lib/x64)

add_subdirectory(${CMAKE_SOURCE_DIR}/lib/rt64 ${CMAKE_BINARY_DIR}/rt64)
add_subdirectory(${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime ${CMAKE_BINARY_DIR}/N64ModernRuntime)

# Recompiler tools (built from lib/N64Recomp @ ffb39cd; the runtime pins 81213c1, whose include/ and
# code generator are identical -- see docs/phase2.md 2.4).
set(N64RECOMP_EXE "${CMAKE_SOURCE_DIR}/lib/N64Recomp/build/N64Recomp.exe" CACHE FILEPATH "N64Recomp executable")
set(RSPRECOMP_EXE "${CMAKE_SOURCE_DIR}/lib/N64Recomp/build/RSPRecomp.exe" CACHE FILEPATH "RSPRecomp executable")
set(BASEROM "${CMAKE_SOURCE_DIR}/baserom.us.z64")

# ---------------------------------------------------------------------------------------------
# Code generation. RecompiledFuncs/ and rsp/aspMain.cpp are generated from the ROM; the C file
# list is globbed with CONFIGURE_DEPENDS so a change in the number of output files reconfigures.
# ---------------------------------------------------------------------------------------------
# WG3D_TRACE=ON (use a separate build dir, e.g. WG3D_BUILD_DIR=build/cmake-trace) compiles the trace_mode
# output in RecompiledFuncsTrace/: every recompiled function calls TRACE_ENTRY()/TRACE_RETURN()
# (include/trace.h), which records per-function coverage and the per-thread trace ring (chunk 4.2).
option(WG3D_TRACE "Build the trace/coverage variant of the recompiled code" OFF)
option(WG3D_CONSOLE "Build wg3d.exe as a console program (development; the default is a GUI program)" OFF)
if(WG3D_TRACE)
    set(RECOMP_DIR ${CMAKE_SOURCE_DIR}/RecompiledFuncsTrace)
    set(GEN_CONFIG_ARGS --trace)
else()
    set(RECOMP_DIR ${CMAKE_SOURCE_DIR}/RecompiledFuncs)
    set(GEN_CONFIG_ARGS --no-check)
endif()

add_custom_command(
    OUTPUT ${RECOMP_DIR}/funcs.h ${CMAKE_SOURCE_DIR}/include/wg3d_skipped.h
    COMMAND ${Python3_EXECUTABLE} tools/gen_config.py ${GEN_CONFIG_ARGS}
    DEPENDS ${CMAKE_SOURCE_DIR}/syms/wg3d.us.syms.toml
            ${CMAKE_SOURCE_DIR}/syms/dead_ignore.txt
            ${CMAKE_SOURCE_DIR}/tools/gen_config.py
            ${CMAKE_SOURCE_DIR}/tools/hw_audit.py
            ${BASEROM}
            ${N64RECOMP_EXE}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Recompiling MIPS code (N64Recomp)"
    VERBATIM)

add_custom_command(
    OUTPUT ${CMAKE_SOURCE_DIR}/rsp/aspMain.cpp
    COMMAND ${RSPRECOMP_EXE} aspMain.toml
    DEPENDS ${CMAKE_SOURCE_DIR}/aspMain.toml ${BASEROM} ${RSPRECOMP_EXE}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Recompiling audio microcode (RSPRecomp)"
    VERBATIM)

add_custom_target(GenerateRecompiled DEPENDS
    ${RECOMP_DIR}/funcs.h
    ${CMAKE_SOURCE_DIR}/include/wg3d_skipped.h
    ${CMAKE_SOURCE_DIR}/rsp/aspMain.cpp)

# ---------------------------------------------------------------------------------------------
# WG3DRecompiled: the recompiled game code plus the recompiled audio microcode.
# ---------------------------------------------------------------------------------------------
# On a clean tree, generate once at configure time so the glob below sees the real file list.
if(NOT EXISTS ${RECOMP_DIR}/funcs.h)
    message(STATUS "${RECOMP_DIR} missing: running tools/gen_config.py ${GEN_CONFIG_ARGS}")
    execute_process(COMMAND ${Python3_EXECUTABLE} tools/gen_config.py ${GEN_CONFIG_ARGS}
                    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} RESULT_VARIABLE GEN_RESULT)
    if(NOT GEN_RESULT EQUAL 0)
        message(FATAL_ERROR "tools/gen_config.py failed")
    endif()
endif()
file(GLOB FUNC_C_SOURCES CONFIGURE_DEPENDS ${RECOMP_DIR}/*.c)
# lookup.cpp: get_entrypoint_address()/get_rom_name(). recomp_overlays.inl (the section/function
# table) is not compiled here; the runtime glue includes it (Phase 3).
file(GLOB FUNC_CXX_SOURCES CONFIGURE_DEPENDS ${RECOMP_DIR}/*.cpp)

add_library(WG3DRecompiled STATIC ${FUNC_C_SOURCES} ${FUNC_CXX_SOURCES} ${CMAKE_SOURCE_DIR}/rsp/aspMain.cpp)
add_dependencies(WG3DRecompiled GenerateRecompiled)

target_include_directories(WG3DRecompiled PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${RECOMP_DIR}
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/librecomp/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/ultramodern/include)

# Generated code: same flags as Zelda64Recomp's RecompiledFuncs, except implicit declarations stay
# enabled (include/wg3d_skipped.h declares every skipped function the code calls; chunk 2.3).
set_source_files_properties(${FUNC_C_SOURCES} PROPERTIES COMPILE_OPTIONS
    "-fno-strict-aliasing;-Wno-unused-variable;-Wno-unused-label;-Wno-unused-but-set-variable")

# RSP vector unit emulation uses SSE4.2 intrinsics (Zelda64Recomp builds it with -march=nehalem).
# -Wno-shift-op-parentheses: librecomp's rsp_vu_impl.hpp writes `result >> 31 - shift`, which is the
# intended (31 - shift) grouping; the warning is in the runtime header, not generated code.
set_source_files_properties(${CMAKE_SOURCE_DIR}/rsp/aspMain.cpp PROPERTIES COMPILE_OPTIONS
    "/clang:-march=nehalem;-Wno-unused-variable;-Wno-unused-label;-Wno-unused-but-set-variable;-Wno-shift-op-parentheses")

# Icon and version resource (chunk 5.2); res/wg3d.ico comes from tools/make_icon.py.
set(WG3D_RES_DIR ${CMAKE_SOURCE_DIR}/res)
configure_file(${CMAKE_SOURCE_DIR}/res/wg3d.rc.in ${CMAKE_BINARY_DIR}/wg3d.rc @ONLY)

# ---------------------------------------------------------------------------------------------
# wg3d: the executable (Phase 3). Glue adapted from Quest64-Recomp; see docs/phase3.md.
# ---------------------------------------------------------------------------------------------
add_executable(wg3d
    ${CMAKE_SOURCE_DIR}/src/main/main.cpp
    ${CMAKE_SOURCE_DIR}/src/main/config.cpp
    ${CMAKE_SOURCE_DIR}/src/main/register_overlays.cpp
    ${CMAKE_SOURCE_DIR}/src/main/rt64_render_context.cpp
    ${CMAKE_SOURCE_DIR}/src/main/trace.cpp
    ${CMAKE_SOURCE_DIR}/src/main/crash_handler.cpp
    ${CMAKE_SOURCE_DIR}/src/main/dl_stats.cpp
    ${CMAKE_SOURCE_DIR}/src/main/capture.cpp
    ${CMAKE_SOURCE_DIR}/src/main/frame_log.cpp
    ${CMAKE_SOURCE_DIR}/src/main/log.cpp
    ${CMAKE_BINARY_DIR}/wg3d.rc
    ${CMAKE_SOURCE_DIR}/src/game/pi.cpp
    ${CMAKE_SOURCE_DIR}/src/game/input.cpp
    ${CMAKE_SOURCE_DIR}/src/game/test_hooks.cpp)

target_include_directories(wg3d PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${RECOMP_DIR}
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/hlslpp/include
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/inc
    ${CMAKE_SOURCE_DIR}/lib/rt64/src
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/rhi
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/render)

# NOMINMAX: <Windows.h> (pulled in by RT64 headers) otherwise breaks std::min/std::max.
target_compile_definitions(wg3d PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS WG3D_VERSION="${PROJECT_VERSION}")
target_link_libraries(wg3d PRIVATE WG3DRecompiled librecomp ultramodern rt64 SDL2 Winmm.lib Dbghelp.lib)

# Identical code folding would merge distinct recompiled functions (Zelda64Recomp does the same).
target_link_options(wg3d PRIVATE /OPT:NOICF)
# Release builds still get a PDB (public symbols for every function, full debug info for our own
# sources) so the crash handler can name recompiled functions. /DEBUG defaults to /OPT:NOREF; keep REF.
target_compile_options(wg3d PRIVATE /Z7)
target_link_options(wg3d PRIVATE /DEBUG /OPT:REF)
# GUI program by default (chunk 5.2): no console window; output goes to logs/wg3d.log (src/main/log.cpp),
# and --console shows it in a console. mainCRTStartup keeps main() as the entry point.
if(WG3D_CONSOLE)
    set_target_properties(wg3d PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")
else()
    set_target_properties(wg3d PROPERTIES LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")
endif()

add_custom_command(TARGET wg3d POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${sdl2_SOURCE_DIR}/lib/x64/SDL2.dll"
        "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxil.dll"
        "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxcompiler.dll"
        $<TARGET_FILE_DIR:wg3d>)
