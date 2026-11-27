# SPDX-License-Identifier: GPL-3.0-or-later
# The release split: xc2_rt.dll holds the runtime and the boot code, and the player's exe is only a stub linked
# against the libraries lifted from their own dump. Upstream builds it the same way (game-build/fefw/CMakeLists.txt
# with FEFW_RT_DLL=ON); the point is that the exe can then be built by Zig with no MSVC and no Windows SDK.
#
#   pwsh tools\build\build-release.ps1          # configure + build the DLL, compile the lift, link the exe
#
# Nothing here is derived from the game: the DLL is our own code plus the runtime, and the lifted libraries are
# imported from the work folder by the build script, never by this project.
cmake_minimum_required(VERSION 3.25)
project(xc2_rt CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static MSVC runtime (/MT), as build-boot.ps1 uses for the exe.
if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

get_filename_component(XC2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
# The build stamp the runtime logs (xc2/src/xc2_boot.cpp): the nearest tag plus the commit, so a player's log says
# which build ran. Outside a git checkout (a source tarball) it says "unknown" rather than a stale literal.
if(NOT DEFINED XC2_BUILD_STAMP)
    execute_process(COMMAND git -C "${XC2_ROOT}" describe --tags --always --dirty
                    OUTPUT_VARIABLE XC2_BUILD_STAMP OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET RESULT_VARIABLE _stamp_rc)
    if(_stamp_rc OR NOT XC2_BUILD_STAMP)
        set(XC2_BUILD_STAMP "unknown")
    endif()
endif()
message(STATUS "xc2 build stamp: ${XC2_BUILD_STAMP}")
set(FEFW "${XC2_ROOT}/third_party/fefw-recomp")
# The headers of the pinned third-party libraries live in the runtime's own build tree (FetchContent).
set(GFX "${FEFW}/build/runtime-gfx")

set(NXRT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NXRT_BUILD_GFX ON CACHE BOOL "" FORCE)
set(NXRT_BUILD_GAMES ON CACHE BOOL "" FORCE)
set(NXRT_GAMES "xc2ns2" CACHE STRING "" FORCE)
set(NXRT_TEST_GAME "xc2ns2" CACHE STRING "" FORCE)
set(NXRT_RECOMP_INCLUDE "${XC2_ROOT}/third_party/nxrecomp/recomp/include" CACHE PATH "")

# libvorbis (BSD-3): the movie player's sound (movie::AudioDecoder). Detected before the runtime's sources are configured, because
# movie_adec.cpp is compiled there and needs the define and the include path.
if(NOT NXRT_VORBIS_ROOT)
    foreach(cand "$ENV{VCPKG_ROOT}/installed/x64-windows-static" "E:/Tools/dev/vcpkg/installed/x64-windows-static" "C:/vcpkg/installed/x64-windows-static")
        if(EXISTS "${cand}/lib/vorbis.lib")
            set(NXRT_VORBIS_ROOT "${cand}")
            break()
        endif()
    endforeach()
endif()
if(NXRT_VORBIS_ROOT)
    add_compile_definitions(NXRT_HAVE_VORBIS=1)
    include_directories("${NXRT_VORBIS_ROOT}/include")
    link_directories("${NXRT_VORBIS_ROOT}/lib")
    message(STATUS "nxrt: libvorbis ${NXRT_VORBIS_ROOT}/lib/vorbis.lib (movie audio)")
else()
    message(STATUS "nxrt: no libvorbis, movie audio is silent")
endif()

add_subdirectory("${FEFW}/runtime" runtime)

add_library(xc2_rt SHARED
    "${XC2_ROOT}/xc2/src/xc2_boot.cpp"
    "${XC2_ROOT}/xc2/src/x2_opus301.cpp"
    "${XC2_ROOT}/xc2/src/x2_audio301.cpp"
    "${XC2_ROOT}/xc2/src/xc2_nav_overlay.cpp"
    "${XC2_ROOT}/xc2/src/xc2_nav_mesh.cpp"
    "${XC2_ROOT}/xc2/src/voice/xc2_voice_db.cpp"
    "${XC2_ROOT}/xc2/src/voice/xc2_voice_director.cpp"
    "${XC2_ROOT}/xc2/src/voice/xc2_voice_renderer.cpp"
    "${XC2_ROOT}/xc2/src/voice/xc2_voice_hooks.cpp"
    "${XC2_ROOT}/xc2/src/voice/xc2_voice_overlay.cpp"
    src/xc2_rt.def)
target_include_directories(xc2_rt PRIVATE
    "${XC2_ROOT}/third_party/nxrecomp/recomp/include"
    "${FEFW}/runtime/include"
    "${FEFW}/runtime/games"
    # play_types.h (the binding engine's vocabulary) lives beside the play sources, not in include/: xc2_boot.cpp
    # registers the game's factory bindings, so it needs this one directory.
    "${FEFW}/runtime/src/gfx/play"
    "${GFX}/_deps/volk-src"
    "${GFX}/_deps/vulkan_headers-src/include"
    "${GFX}/_deps/vma-src/include"
    "${GFX}/_deps/sdl3-src/include")
target_compile_definitions(xc2_rt PRIVATE XC2_RT_DLL=1 XC2_MODULES_7=1 XC2_BUILD_STAMP="${XC2_BUILD_STAMP}")
if(NXRT_VORBIS_ROOT)
    target_link_libraries(xc2_rt PRIVATE "${NXRT_VORBIS_ROOT}/lib/vorbis.lib" "${NXRT_VORBIS_ROOT}/lib/ogg.lib")
endif()

target_link_libraries(xc2_rt PRIVATE
    nxrt_game_xc2ns2_core nxrt_gfx nxrt
    kernel32 user32 gdi32 winmm imm32 ole32 oleaut32 version uuid advapi32 setupapi shell32 dinput8 psapi)
# No /DEBUG: a release DLL should not carry a PDB path. /HIGHENTROPYVA:NO keeps the guest ranges at 0x71_0000_0000
# free, which is what the runtime needs to map the game's address space.
target_link_options(xc2_rt PRIVATE /HIGHENTROPYVA:NO /DEBUG "/PDB:${CMAKE_CURRENT_BINARY_DIR}/xc2_rt.pdb" "/MAP:${CMAKE_CURRENT_BINARY_DIR}/xc2_rt.map")
# A local diagnostic build (not shipped): keep a PDB so a freeze dump's xc2_rt.dll+0x... frames can be named with llvm-pdbutil.
# A freeze report alone says which host function a stuck thread is in, which is otherwise guesswork.
option(NXRT_DLL_SYMBOLS "Link xc2_rt.dll with a PDB for the crash dump's offsets" OFF)
if(NXRT_DLL_SYMBOLS)
    target_link_options(xc2_rt PRIVATE /DEBUG "/PDB:${CMAKE_CURRENT_BINARY_DIR}/xc2_rt.pdb")
endif()