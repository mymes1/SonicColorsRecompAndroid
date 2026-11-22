cmake_minimum_required(VERSION 3.21)
# Let MSVC_DEBUG_INFORMATION_FORMAT control /Zi vs /Z7 (needed so ppcrecomp can avoid /Zi OOM).
if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
endif()

# Build with the presets in CMakePresets.json: clang-cl + Ninja on Windows (MSVC also
# works), clang + Ninja on Linux, MinGW-w64 cross from Linux (windows-mingw). The runtime
# keeps its platform code in runtime/platform: Win32, Direct3D 11, XInput and XAudio2 on
# Windows; SDL3, dxvk-native (Direct3D 11 on Vulkan) and vkd3d-shader on Linux
# (docs/linux.md, docs/windows-cross.md).
project(SonicColorsRecomp LANGUAGES CXX)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(LINUX ON)
    enable_language(C)   # the libraries built from source (SDL3, vkd3d, libarchive, ...)
elseif(MINGW)
    enable_language(C)   # ExternalProject zlib/xz/libarchive for the MinGW target
endif()

# Output directory: Linux/native Windows use out/; Linux→Windows cross uses out-windows/
# (override with -DSCR_OUT_DIR=...). Never mix host and cross artifacts in one folder.
if(NOT SCR_OUT_DIR)
    if(CMAKE_CROSSCOMPILING AND WIN32)
        set(SCR_OUT_DIR ${CMAKE_SOURCE_DIR}/out-windows)
    else()
        set(SCR_OUT_DIR ${CMAKE_SOURCE_DIR}/out)
    endif()
endif()
file(MAKE_DIRECTORY ${SCR_OUT_DIR})

# MSVC: use 23 so CMake emits a single /std flag. Mixing CMAKE_CXX_STANDARD 20 (/std:c++20)
# with per-target /std:c++latest caused D9025 spam on every TU.
if(MSVC)
    set(CMAKE_CXX_STANDARD 23)
else()
    set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# SCR_CALL_COUNTS: instrumented build counting calls to every game function, for
# frame-rate analysis with TAS scripts (docs/tas.md); the exe is SonicColorsRecomp_counts.
option(SCR_CALL_COUNTS "Count calls to every recompiled function" OFF)
if(SCR_CALL_COUNTS)
    add_compile_definitions(SCR_CALL_COUNTS)
endif()

# MSVC_FRONTEND: cl or clang-cl (MSVC-style flags); CLANG: clang-cl or clang
set(MSVC_FRONTEND ${MSVC})
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(CLANG ON)
endif()

# Visual Studio 2026 is recommended; 2022 (MSVC 19.30) is the oldest supported. clang-cl
# reports the version of the Visual Studio libraries it found.
if(MSVC_FRONTEND)
    if(CLANG)
        set(VS_LIB_VERSION ${CMAKE_CXX_SIMULATE_VERSION})
    else()
        set(VS_LIB_VERSION ${CMAKE_CXX_COMPILER_VERSION})
    endif()
    if(VS_LIB_VERSION VERSION_LESS 19.30)
        message(FATAL_ERROR "Visual Studio 2022 or newer is required (found MSVC ${VS_LIB_VERSION}); "
                            "install Visual Studio 2026 (Build Tools are enough) with the C++ workload")
    endif()
endif()

# ---------------------------------------------------------------- disc image installer
# nod (https://github.com/encounter/nod, MIT OR Apache-2.0, used under MIT) reads the
# user's disc image; the prebuilt package is downloaded at configure time.
include(FetchContent)
set(NOD_VERSION v2.0.0-alpha.12)
if(WIN32 AND NOT MINGW)
    FetchContent_Declare(nod_prebuilt
        URL https://github.com/encounter/nod/releases/download/${NOD_VERSION}/libnod-windows-x86_64.tar.gz
        URL_HASH SHA256=c9dfecf8e52066a5222f3cb1cad18d0740c958ce65bf60cb84e780a3856ed881
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
elseif(LINUX)
    FetchContent_Declare(nod_prebuilt
        URL https://github.com/encounter/nod/releases/download/${NOD_VERSION}/libnod-linux-x86_64.tar.gz
        URL_HASH SHA256=920adb1d2386c8728e0521f8d52809057c04bd46913433a0000ee0931d08d69f
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
endif()
if(MINGW)
    # MSVC prebuilt nod_static.lib / nod.dll need the VC++ runtime. Build windows-gnu instead.
    include(cmake/nod_mingw.cmake)
elseif(WIN32 OR LINUX)
    FetchContent_MakeAvailable(nod_prebuilt)
    find_package(nod REQUIRED CONFIG PATHS ${nod_prebuilt_SOURCE_DIR} NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    set(SCR_NOD_LINK nod::nod_static)
endif()
if(WIN32 OR LINUX)

    add_library(scr_installer STATIC runtime/install/install.cpp runtime/install/verify.cpp runtime/install/releases.cpp)
    target_include_directories(scr_installer PUBLIC runtime)
    target_link_libraries(scr_installer PUBLIC ${SCR_NOD_LINK})
    if(WIN32)
        target_compile_definitions(scr_installer PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
        target_sources(scr_installer PRIVATE runtime/platform/win32/sha1.cpp)
        target_link_libraries(scr_installer PUBLIC bcrypt)   # Sha1 via BCrypt
        if(MINGW)
            target_compile_options(scr_installer PRIVATE -include ${CMAKE_SOURCE_DIR}/runtime/platform/compat.h)
        endif()
    else()
        target_compile_options(scr_installer PRIVATE -include ${CMAKE_SOURCE_DIR}/runtime/platform/compat.h)
        target_sources(scr_installer PRIVATE runtime/platform/linux/sha1.cpp)
        target_link_libraries(scr_installer PUBLIC pthread dl m)
    endif()
    if(MSVC_FRONTEND)
        target_compile_options(scr_installer PRIVATE /EHsc /utf-8)
        # nod's C dependencies are built against the DLL runtime; we link the static one
        target_link_options(scr_installer INTERFACE /NODEFAULTLIB:msvcrt /IGNORE:4217)
    endif()

    # scr_install <image>: installs game/extracted/DATA before the first build_generated.py
    add_executable(scr_install tools/scr_install/main.cpp)
    target_link_libraries(scr_install PRIVATE scr_installer)
    if(MSVC_FRONTEND)
        target_compile_options(scr_install PRIVATE /EHsc /utf-8)
    endif()
    set_target_properties(scr_install PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${SCR_OUT_DIR}
                                                 RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${SCR_OUT_DIR}
                                                 RUNTIME_OUTPUT_DIRECTORY_RELEASE ${SCR_OUT_DIR})
    if(MINGW)
        # CMake defaults Win32 exes to the GUI subsystem under some toolchains; force console.
        set_target_properties(scr_install PROPERTIES WIN32_EXECUTABLE FALSE)
        # -municode: entry is wmain (same as MSVC); without it MinGW looks for WinMain/main.
        target_link_options(scr_install PRIVATE -mconsole -municode
                            -static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic)
        if(SCR_NOD_DLL)
            add_custom_command(TARGET scr_install POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SCR_NOD_DLL}" $<TARGET_FILE_DIR:scr_install>
                VERBATIM)
        endif()
    endif()

    # The Android installer (runtime/platform/android/iso_install.cpp) reads disc images
    # without nod, plain ISO or a ZIP around one, so it gets its own test against a synthetic
    # disc image. Host build only: the same sources are compiled into the APK for arm64.
    add_executable(scr_iso_install_test EXCLUDE_FROM_ALL
        tools/iso_install_test/main.cpp
        runtime/platform/android/iso_install.cpp
        runtime/platform/android/deflate.cpp
        runtime/platform/aes128.cpp
        runtime/platform/linux/sha1.cpp
        runtime/install/releases.cpp)
    target_compile_definitions(scr_iso_install_test PRIVATE SCR_ISO_INSTALL_TEST)
    target_include_directories(scr_iso_install_test PRIVATE runtime runtime/platform)
    target_link_libraries(scr_iso_install_test PRIVATE pthread)
    if(NOT WIN32)
        target_compile_options(scr_iso_install_test PRIVATE -include ${CMAKE_SOURCE_DIR}/runtime/platform/compat.h)
    endif()
    set_target_properties(scr_iso_install_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${SCR_OUT_DIR}
                                                          RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${SCR_OUT_DIR}
                                                          RUNTIME_OUTPUT_DIRECTORY_RELEASE ${SCR_OUT_DIR})

    # The Android PNG codec (runtime/platform/android/image.cpp) replaces SDL3/WIC, so it is
    # checked against fixed pixel references generated independently by Python's zlib.
    add_executable(scr_image_test EXCLUDE_FROM_ALL
        tools/image_test/main.cpp
        runtime/platform/android/image.cpp
        runtime/platform/android/deflate.cpp)
    target_include_directories(scr_image_test PRIVATE runtime runtime/platform)
    target_compile_definitions(scr_image_test PRIVATE SCR_ANDROID)
    if(NOT WIN32)
        target_compile_options(scr_image_test PRIVATE -include ${CMAKE_SOURCE_DIR}/runtime/platform/compat.h)
    endif()
    set_target_properties(scr_image_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${SCR_OUT_DIR}
                                                     RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${SCR_OUT_DIR}
                                                     RUNTIME_OUTPUT_DIRECTORY_RELEASE ${SCR_OUT_DIR})

    # Disc-path helpers + unsupported-region copy (no image I/O).
    add_executable(scr_install_helpers_test EXCLUDE_FROM_ALL tools/install_helpers_test/main.cpp)
    target_link_libraries(scr_install_helpers_test PRIVATE scr_installer)
    if(LINUX)
        target_compile_options(scr_install_helpers_test PRIVATE -include ${CMAKE_SOURCE_DIR}/runtime/platform/compat.h)
        target_link_options(scr_install_helpers_test PRIVATE -static-libstdc++ -static-libgcc)
    endif()
    if(MSVC_FRONTEND)
        target_compile_options(scr_install_helpers_test PRIVATE /EHsc /utf-8)
    endif()
    if(MINGW)
        set_target_properties(scr_install_helpers_test PROPERTIES WIN32_EXECUTABLE FALSE)
        target_link_options(scr_install_helpers_test PRIVATE -mconsole
                            -static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic)
    endif()
    set_target_properties(scr_install_helpers_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${SCR_OUT_DIR}
                                                              RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${SCR_OUT_DIR}
                                                              RUNTIME_OUTPUT_DIRECTORY_RELEASE ${SCR_OUT_DIR})

    add_executable(scr_release_notes_test EXCLUDE_FROM_ALL tools/release_notes_test/main.cpp)
    if(LINUX)
        target_compile_options(scr_release_notes_test PRIVATE -include ${CMAKE_SOURCE_DIR}/runtime/platform/compat.h)
        target_link_options(scr_release_notes_test PRIVATE -static-libstdc++ -static-libgcc)
    endif()
    if(MSVC_FRONTEND)
        target_compile_options(scr_release_notes_test PRIVATE /EHsc /utf-8)
    endif()
    if(MINGW)
        set_target_properties(scr_release_notes_test PROPERTIES WIN32_EXECUTABLE FALSE)
        target_link_options(scr_release_notes_test PRIVATE -mconsole
                            -static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic)
    endif()
    set_target_properties(scr_release_notes_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${SCR_OUT_DIR}
                                                            RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${SCR_OUT_DIR}
                                                            RUNTIME_OUTPUT_DIRECTORY_RELEASE ${SCR_OUT_DIR})

    include(cmake/mod_deps.cmake)

    add_executable(scr_music_paths_test EXCLUDE_FROM_ALL tools/music_paths_test/main.cpp
                                         runtime/audio/music_paths.cpp)
    target_include_directories(scr_music_paths_test PRIVATE runtime)
    target_link_libraries(scr_music_paths_test PRIVATE scr_json)
    if(LINUX)
        target_compile_options(scr_music_paths_test PRIVATE -include ${CMAKE_SOURCE_DIR}/runtime/platform/compat.h)
        target_link_options(scr_music_paths_test PRIVATE -static-libstdc++ -static-libgcc)
    endif()
    if(MSVC_FRONTEND)
        target_compile_options(scr_music_paths_test PRIVATE /EHsc /utf-8)
    endif()
    if(MINGW)
        set_target_properties(scr_music_paths_test PROPERTIES WIN32_EXECUTABLE FALSE)
        target_compile_options(scr_music_paths_test PRIVATE -include ${CMAKE_SOURCE_DIR}/runtime/platform/compat.h)
        target_link_options(scr_music_paths_test PRIVATE -mconsole
                            -static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic)
    endif()
    set_target_properties(scr_music_paths_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${SCR_OUT_DIR}
                                                          RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${SCR_OUT_DIR}
                                                          RUNTIME_OUTPUT_DIRECTORY_RELEASE ${SCR_OUT_DIR})

    add_executable(scr_update_apply_test EXCLUDE_FROM_ALL tools/update_apply_test/main.cpp
                                          runtime/ui/update_apply.cpp)
    target_link_libraries(scr_update_apply_test PRIVATE scr_archive)
    target_include_directories(scr_update_apply_test PRIVATE runtime)
    if(LINUX)
        target_compile_options(scr_update_apply_test PRIVATE -include ${CMAKE_SOURCE_DIR}/runtime/platform/compat.h)
        target_link_options(scr_update_apply_test PRIVATE -static-libstdc++ -static-libgcc)
    endif()
    if(MSVC_FRONTEND)
        target_compile_options(scr_update_apply_test PRIVATE /EHsc /utf-8)
    endif()
    if(MINGW)
        set_target_properties(scr_update_apply_test PROPERTIES WIN32_EXECUTABLE FALSE)
        target_compile_options(scr_update_apply_test PRIVATE -include ${CMAKE_SOURCE_DIR}/runtime/platform/compat.h)
        target_link_options(scr_update_apply_test PRIVATE -mconsole
                            -static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic)
    endif()
    if(WIN32)
        target_link_libraries(scr_update_apply_test PRIVATE bcrypt)
    endif()
    set_target_properties(scr_update_apply_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${SCR_OUT_DIR}
                                                           RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${SCR_OUT_DIR}
                                                           RUNTIME_OUTPUT_DIRECTORY_RELEASE ${SCR_OUT_DIR})

    # scr_mods_test: checks the mod overlay of the disc layer on a synthetic disc (tools/mods_test)
    if(WIN32)
        set(SCR_PLATFORM_DIR runtime/platform/win32)
    else()
        set(SCR_PLATFORM_DIR runtime/platform/linux)
    endif()
    add_executable(scr_mods_test EXCLUDE_FROM_ALL tools/mods_test/main.cpp runtime/ios/disc.cpp runtime/mods/mods.cpp
                                 runtime/mods/mod_install.cpp runtime/mods/gamebanana.cpp runtime/net/http.cpp
                                 ${SCR_PLATFORM_DIR}/http.cpp ${SCR_PLATFORM_DIR}/image_decode.cpp)
    target_link_libraries(scr_mods_test PRIVATE scr_archive scr_json)
    target_include_directories(scr_mods_test PRIVATE runtime)
    target_compile_definitions(scr_mods_test PRIVATE SCR_MODS_TEST_DATA="${CMAKE_SOURCE_DIR}/tools/mods_test/data")
    if(WIN32)
        target_link_libraries(scr_mods_test PRIVATE winhttp bcrypt shell32 windowscodecs ole32)
        target_compile_definitions(scr_mods_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
        if(MINGW)
            target_compile_options(scr_mods_test PRIVATE -include ${CMAKE_SOURCE_DIR}/runtime/platform/compat.h)
            target_link_options(scr_mods_test PRIVATE -static-libstdc++ -static-libgcc)
        endif()
    else()
        target_link_libraries(scr_mods_test PRIVATE dl)
        target_link_options(scr_mods_test PRIVATE -static-libstdc++ -static-libgcc)   # as the game: runs on older systems
        target_compile_options(scr_mods_test PRIVATE -include ${CMAKE_SOURCE_DIR}/runtime/platform/compat.h)
    endif()
    if(MSVC_FRONTEND)
        target_compile_options(scr_mods_test PRIVATE /EHsc /utf-8)
    endif()
    set_target_properties(scr_mods_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${SCR_OUT_DIR}
                                                   RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${SCR_OUT_DIR}
                                                   RUNTIME_OUTPUT_DIRECTORY_RELEASE ${SCR_OUT_DIR})

    add_executable(scr_music_manifest_test EXCLUDE_FROM_ALL tools/music_manifest_test/main.cpp
                                           runtime/audio/music_manifest.cpp runtime/audio/wav_file.cpp)
    target_include_directories(scr_music_manifest_test PRIVATE runtime)
    if(LINUX)
        target_compile_options(scr_music_manifest_test PRIVATE -include ${CMAKE_SOURCE_DIR}/runtime/platform/compat.h)
        target_link_options(scr_music_manifest_test PRIVATE -static-libstdc++ -static-libgcc)
    endif()
    if(MSVC_FRONTEND)
        target_compile_options(scr_music_manifest_test PRIVATE /EHsc /utf-8)
    endif()
    set_target_properties(scr_music_manifest_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${SCR_OUT_DIR}
                                                             RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${SCR_OUT_DIR}
                                                             RUNTIME_OUTPUT_DIRECTORY_RELEASE ${SCR_OUT_DIR})

    add_executable(scr_nand_fs_test EXCLUDE_FROM_ALL tools/nand_fs_test/main.cpp runtime/ios/fs_atomic.cpp)
    target_include_directories(scr_nand_fs_test PRIVATE runtime)
    if(LINUX)
        target_compile_options(scr_nand_fs_test PRIVATE -include ${CMAKE_SOURCE_DIR}/runtime/platform/compat.h)
        target_link_options(scr_nand_fs_test PRIVATE -static-libstdc++ -static-libgcc)
    endif()
    if(MSVC_FRONTEND)
        target_compile_options(scr_nand_fs_test PRIVATE /EHsc /utf-8)
    endif()
    set_target_properties(scr_nand_fs_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${SCR_OUT_DIR}
                                                      RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${SCR_OUT_DIR}
                                                      RUNTIME_OUTPUT_DIRECTORY_RELEASE ${SCR_OUT_DIR})

    # Header-only nand_paths backup/restore (tests use rootOverride; no IOS/platform link)
    add_executable(scr_nand_backup_test EXCLUDE_FROM_ALL tools/nand_backup_test/main.cpp)
    target_include_directories(scr_nand_backup_test PRIVATE runtime)
    if(LINUX)
        target_compile_options(scr_nand_backup_test PRIVATE -include ${CMAKE_SOURCE_DIR}/runtime/platform/compat.h -std=c++20)
        target_link_options(scr_nand_backup_test PRIVATE -static-libstdc++ -static-libgcc)
    endif()
    if(MSVC_FRONTEND)
        target_compile_options(scr_nand_backup_test PRIVATE /EHsc /utf-8)
    endif()
    set_target_properties(scr_nand_backup_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${SCR_OUT_DIR}
                                                          RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${SCR_OUT_DIR}
                                                          RUNTIME_OUTPUT_DIRECTORY_RELEASE ${SCR_OUT_DIR})

    # music_asset host launcher (Windows redistributable; optional on Linux — bash wrapper remains default)
    add_executable(music_asset tools/music_asset_launcher.cpp)
    target_compile_features(music_asset PRIVATE cxx_std_20)
    if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_compile_definitions(music_asset PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        if(MINGW)
            target_compile_options(music_asset PRIVATE -include ${CMAKE_SOURCE_DIR}/runtime/platform/compat.h)
            target_link_options(music_asset PRIVATE -static-libstdc++ -static-libgcc)
        endif()
    else()
        target_compile_options(music_asset PRIVATE -include ${CMAKE_SOURCE_DIR}/runtime/platform/compat.h)
        target_link_options(music_asset PRIVATE -static-libstdc++ -static-libgcc)
        # Avoid clobbering the POSIX tools/music_asset bash script in the source tree.
        set_target_properties(music_asset PROPERTIES OUTPUT_NAME music_asset_launcher)
    endif()
    set_target_properties(music_asset PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${SCR_OUT_DIR}
                                                 RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${SCR_OUT_DIR}
                                                 RUNTIME_OUTPUT_DIRECTORY_RELEASE ${SCR_OUT_DIR})

    # Platform stubs live in the test; only music_tool.cpp is linked from the runtime.
    add_executable(scr_platform_test EXCLUDE_FROM_ALL tools/platform_test/main.cpp
                                       runtime/audio/music_tool.cpp)
    target_include_directories(scr_platform_test PRIVATE runtime)
    if(LINUX OR MINGW)
        target_compile_options(scr_platform_test PRIVATE -include ${CMAKE_SOURCE_DIR}/runtime/platform/compat.h)
        target_link_options(scr_platform_test PRIVATE -static-libstdc++ -static-libgcc)
    endif()
    if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_compile_definitions(scr_platform_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    if(MSVC_FRONTEND)
        target_compile_options(scr_platform_test PRIVATE /EHsc /utf-8)
    endif()
    set_target_properties(scr_platform_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${SCR_OUT_DIR}
                                                        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${SCR_OUT_DIR}
                                                        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${SCR_OUT_DIR})

    # Real Platform_RunProcess implementation (not stubs).
    add_executable(scr_run_process_test EXCLUDE_FROM_ALL tools/run_process_test/main.cpp
                                       runtime/platform/run_process.cpp)
    target_include_directories(scr_run_process_test PRIVATE runtime)
    if(LINUX OR MINGW)
        target_compile_options(scr_run_process_test PRIVATE -include ${CMAKE_SOURCE_DIR}/runtime/platform/compat.h)
        target_link_options(scr_run_process_test PRIVATE -static-libstdc++ -static-libgcc)
    endif()
    if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_compile_definitions(scr_run_process_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    if(MSVC_FRONTEND)
        target_compile_options(scr_run_process_test PRIVATE /EHsc /utf-8)
    endif()
    if(MINGW)
        set_target_properties(scr_run_process_test PROPERTIES WIN32_EXECUTABLE FALSE)
        target_link_options(scr_run_process_test PRIVATE -mconsole
                            -static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic)
    endif()
    set_target_properties(scr_run_process_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${SCR_OUT_DIR}
                                                          RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${SCR_OUT_DIR}
                                                          RUNTIME_OUTPUT_DIRECTORY_RELEASE ${SCR_OUT_DIR})

    add_executable(scr_config_test EXCLUDE_FROM_ALL tools/config_test/main.cpp runtime/config.cpp)
    target_include_directories(scr_config_test PRIVATE runtime)
    if(LINUX)
        target_compile_options(scr_config_test PRIVATE -include ${CMAKE_SOURCE_DIR}/runtime/platform/compat.h)
        target_link_options(scr_config_test PRIVATE -static-libstdc++ -static-libgcc)
    endif()
    if(MSVC_FRONTEND)
        target_compile_options(scr_config_test PRIVATE /EHsc /utf-8)
    endif()
    set_target_properties(scr_config_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${SCR_OUT_DIR}
                                                     RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${SCR_OUT_DIR}
                                                     RUNTIME_OUTPUT_DIRECTORY_RELEASE ${SCR_OUT_DIR})
endif()

# ---------------------------------------------------------------- Linux platform libraries
# Downloaded at configure time, pinned like nod and ImGui; their shared libraries are copied
# next to the executable (out/) and into the AppImage (tools/package_linux.sh).
#   SDL3 (https://libsdl.org, zlib): window, input, audio, dialogs
#   dxvk-native (https://github.com/doitsujin/dxvk, zlib): Direct3D 11 on Vulkan, prebuilt
#     for the Steam Runtime; its MinGW Windows/DirectX headers are LGPL-2.1+ (data
#     structure definitions only, see its include/dxvk/README.md)
#   vkd3d-shader (https://gitlab.winehq.org/wine/vkd3d, LGPL-2.1+): compiles the generated
#     HLSL to DXBC (the D3DCompile of Wine and Proton), linked statically
if(LINUX)
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
        URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
    FetchContent_MakeAvailable(SDL3)

    FetchContent_Declare(dxvk_native
        URL https://github.com/doitsujin/dxvk/releases/download/v3.1.1/dxvk-native-3.1.1-steamrt-sniper.tar.gz
        URL_HASH SHA256=506fa1ea5bb6ce9a89e11436f7ab5191bc0196e8a577e0cf0b16a45bbc9991b7
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
    FetchContent_MakeAvailable(dxvk_native)   # the archive's usr/ becomes the source dir
    set(DXVK_LIBS ${dxvk_native_SOURCE_DIR}/lib/libdxvk_d3d11.so.0 ${dxvk_native_SOURCE_DIR}/lib/libdxvk_dxgi.so.0)
    add_library(dxvk INTERFACE)
    target_include_directories(dxvk SYSTEM INTERFACE ${dxvk_native_SOURCE_DIR}/include/dxvk)
    target_link_libraries(dxvk INTERFACE ${DXVK_LIBS})

    # autotools build; needs bison, flex and Perl's JSON module. It wants newer Vulkan and
    # SPIR-V headers than older distributions have (Khronos, Apache-2.0/MIT, headers only).
    FetchContent_Declare(vulkan_headers
        URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.341.0.tar.gz
        URL_HASH SHA256=d73bc5036b6556b741f6985ff600ca720308c5f2850e4a43ceb498bd3de069e7
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
        SOURCE_SUBDIR none)   # headers only: no add_subdirectory
    FetchContent_Declare(spirv_headers
        URL https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/vulkan-sdk-1.4.341.0.tar.gz
        URL_HASH SHA256=cab0a654c4917e16367483296b44cdb1d614e3120c721beafcd37e3a8580486c
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
        SOURCE_SUBDIR none)
    FetchContent_MakeAvailable(vulkan_headers spirv_headers)
    include(ExternalProject)
    ExternalProject_Add(vkd3d_build
        URL https://dl.winehq.org/vkd3d/source/vkd3d-1.19.tar.xz
        URL_HASH SHA256=034613605baab8ba84674f8d272cf22b5e86bc6bc03fc5728ef9bce07308baa6
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE   # the archive's times: else make reruns autotools
        CONFIGURE_COMMAND <SOURCE_DIR>/configure --disable-tests --disable-demos --disable-doxygen-doc --enable-static
                          --without-ncurses --without-opengl --without-xcb --with-spirv-tools=no
                          CC=${CMAKE_C_COMPILER} "CFLAGS=-O2 -fPIC -fno-lto"
                          "CPPFLAGS=-I${vulkan_headers_SOURCE_DIR}/include -I${spirv_headers_SOURCE_DIR}/include"
        BUILD_COMMAND make -j8
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS <BINARY_DIR>/.libs/libvkd3d-shader.a)
    ExternalProject_Get_Property(vkd3d_build SOURCE_DIR BINARY_DIR)
    file(MAKE_DIRECTORY ${SOURCE_DIR}/include)
    add_library(vkd3d_shader INTERFACE)
    add_dependencies(vkd3d_shader vkd3d_build)
    target_include_directories(vkd3d_shader SYSTEM INTERFACE ${SOURCE_DIR}/include)
    target_link_libraries(vkd3d_shader INTERFACE ${BINARY_DIR}/.libs/libvkd3d-shader.a m pthread)
endif()

# ---------------------------------------------------------------- Dear ImGui (launcher, settings overlay)
# https://github.com/ocornut/imgui (MIT), downloaded at configure time
if(WIN32 OR LINUX)
    FetchContent_Declare(imgui
        URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz
        URL_HASH SHA256=21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
    FetchContent_MakeAvailable(imgui)
    add_library(imgui STATIC ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
                             ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
                             ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
    target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
    if(WIN32)
        target_sources(imgui PRIVATE ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp)
    else()
        target_sources(imgui PRIVATE ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp)
        target_link_libraries(imgui PUBLIC SDL3::SDL3 dxvk)
    endif()
endif()

if(NOT EXISTS ${CMAKE_SOURCE_DIR}/generated/sources.cmake)
    # Reconfigure by itself once tools/build_generated.py has filled generated/
    file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/generated)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/generated)
    message(WARNING "generated/ is empty: only scr_install is built. Install the game data "
                    "(out/scr_install <disc image>), run tools/build_generated.py, then build again.")
    return()
endif()
include(${CMAKE_SOURCE_DIR}/generated/sources.cmake)

# ---------------------------------------------------------------- recompiled game code
add_library(ppcrecomp OBJECT ${PPC_RECOMP_SOURCES})
target_include_directories(ppcrecomp PUBLIC runtime/cpu generated)
if(MSVC_FRONTEND)
    # Always optimized (unoptimized recomp is unusable). Avoid /Zi (shared PDB) + /Ob2 on these
    # multi‑MB TUs — MSVC hits C1002 "out of heap space in pass 2" under parallel RelWithDebInfo.
    # /Z7 embeds debug info per .obj; /Ob1 keeps inlining lighter. Prefer clang-cl if available.
    target_compile_options(ppcrecomp PRIVATE /O2 /Ob1 /GS- /EHs-c- /Z7)
    if(CLANG)
        target_compile_options(ppcrecomp PRIVATE -Wno-unused-label -Wno-unused-variable)
    else()
        target_compile_options(ppcrecomp PRIVATE /bigobj /wd4102 /wd4244 /wd4146 /wd4018)
    endif()
    # Ensure config-default /Zi does not also apply (CMake may still inject it for RelWithDebInfo).
    set_property(TARGET ppcrecomp PROPERTY MSVC_DEBUG_INFORMATION_FORMAT Embedded)
else()
    target_compile_options(ppcrecomp PRIVATE -O2 -fno-exceptions -fno-strict-aliasing -w)
    if(MINGW)
        # COFF section limit; several recomp TUs exceed the default object size.
        target_compile_options(ppcrecomp PRIVATE -Wa,-mbig-obj)
    endif()
endif()

if(NOT WIN32 AND NOT LINUX)
    message(STATUS "The runtime supports Windows and Linux: building the recompiled code (ppcrecomp) only")
    return()
endif()

# ---------------------------------------------------------------- runtime
# shared code, plus one platform's implementation of runtime/platform/*.h
file(GLOB_RECURSE RUNTIME_SOURCES CONFIGURE_DEPENDS runtime/*.cpp)
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/runtime/(install|platform)/")
if(WIN32)
    file(GLOB PLATFORM_SOURCES CONFIGURE_DEPENDS runtime/platform/win32/*.cpp)
else()
    file(GLOB PLATFORM_SOURCES CONFIGURE_DEPENDS runtime/platform/linux/*.cpp)
endif()
list(FILTER PLATFORM_SOURCES EXCLUDE REGEX "/sha1[.]cpp$")   # in scr_installer
list(APPEND RUNTIME_SOURCES ${PLATFORM_SOURCES})
list(APPEND RUNTIME_SOURCES runtime/platform/run_process.cpp)
add_executable(SonicColorsRecomp ${RUNTIME_SOURCES})
target_include_directories(SonicColorsRecomp PRIVATE runtime runtime/cpu generated)
target_sources(SonicColorsRecomp PRIVATE $<TARGET_OBJECTS:ppcrecomp>)
# version shown in the launcher and the overlay, refreshed on every build
set(SCR_VERSION_CPP ${CMAKE_BINARY_DIR}/scr_version.cpp)
add_custom_target(scr_version
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DVERSION_OUT=${SCR_VERSION_CPP}
            -P ${CMAKE_SOURCE_DIR}/tools/version.cmake
    BYPRODUCTS ${SCR_VERSION_CPP} VERBATIM)
target_sources(SonicColorsRecomp PRIVATE ${SCR_VERSION_CPP})
add_dependencies(SonicColorsRecomp scr_version)
if(WIN32)
    # MinGW ships xaudio2_8/xaudio2_9 import libs, not a bare libxaudio2.a.
    if(MINGW)
        set(SCR_XAUDIO_LIB xaudio2_9)
    else()
        set(SCR_XAUDIO_LIB xaudio2)
    endif()
    target_link_libraries(SonicColorsRecomp PRIVATE scr_installer imgui scr_archive scr_json winhttp windowscodecs bcrypt d3d11 dxgi d3dcompiler xinput winmm ole32 uuid shell32 psapi ${SCR_XAUDIO_LIB} dbghelp dwmapi)
else()
    target_link_libraries(SonicColorsRecomp PRIVATE scr_installer imgui scr_archive scr_json SDL3::SDL3 dxvk vkd3d_shader pthread dl)
endif()
# Linux/native Windows: out/. Cross MinGW: out-windows/ (SCR_OUT_DIR).
set_target_properties(SonicColorsRecomp PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${SCR_OUT_DIR}
                                                   RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${SCR_OUT_DIR}
                                                   RUNTIME_OUTPUT_DIRECTORY_RELEASE ${SCR_OUT_DIR})
if(SCR_CALL_COUNTS)
    set_target_properties(SonicColorsRecomp PROPERTIES OUTPUT_NAME SonicColorsRecomp_counts)
endif()
# licences of the libraries in the exe, shipped with it (package scripts copy licenses/)
set(SCR_LICENSES ${CMAKE_BINARY_DIR}/licenses)
file(DOWNLOAD https://raw.githubusercontent.com/encounter/nod/v2.0.0-alpha.12/LICENSE-MIT ${SCR_LICENSES}/nod-LICENSE-MIT.txt
     EXPECTED_HASH SHA256=544ffe9befaefa6b59ab1fdde58789069ba7c1b65432bdea273a16b2117909b7)
file(DOWNLOAD https://raw.githubusercontent.com/nlohmann/json/v3.12.0/LICENSE.MIT ${SCR_LICENSES}/nlohmann-json-LICENSE.MIT.txt
     EXPECTED_HASH SHA256=46a65cffd1ea955132d95a8dd921640714a8d6b537d2e4e482d31145ae95b603)
add_custom_command(TARGET SonicColorsRecomp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SCR_OUT_DIR}/licenses
    COMMAND ${CMAKE_COMMAND} -E copy ${SCR_LICENSES}/nod-LICENSE-MIT.txt ${SCR_LICENSES}/nlohmann-json-LICENSE.MIT.txt
            ${SCR_OUT_DIR}/licenses/
    COMMAND ${CMAKE_COMMAND} -E copy ${imgui_SOURCE_DIR}/LICENSE.txt ${SCR_OUT_DIR}/licenses/imgui-LICENSE.txt
    COMMAND ${CMAKE_COMMAND} -E copy ${SCR_DEPS_PREFIX}/libarchive/src/scr_libarchive/COPYING ${SCR_OUT_DIR}/licenses/libarchive-COPYING.txt
    COMMAND ${CMAKE_COMMAND} -E copy ${SCR_DEPS_PREFIX}/zlib/src/scr_zlib/LICENSE ${SCR_OUT_DIR}/licenses/zlib-LICENSE.txt
    COMMAND ${CMAKE_COMMAND} -E copy ${SCR_DEPS_PREFIX}/xz/src/scr_lzma/COPYING.0BSD ${SCR_OUT_DIR}/licenses/liblzma-COPYING.0BSD.txt
    VERBATIM)
if(WIN32)
    target_compile_definitions(SonicColorsRecomp PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()
if(MSVC_FRONTEND)
    # /std comes from CMAKE_CXX_STANDARD (23) — do not also pass /std:c++latest (D9025).
    target_compile_options(SonicColorsRecomp PRIVATE /EHsc /utf-8 $<$<CONFIG:RelWithDebInfo>:/Ob2>)
    if(CLANG)
        target_compile_options(SonicColorsRecomp PRIVATE -Wno-unused-function -Wno-unused-variable)
    else()
        target_compile_options(SonicColorsRecomp PRIVATE /wd4611)
    endif()
    # NOICF: identical COMDAT folding can merge recomp thunks and break originals/crash maps.
    target_link_options(SonicColorsRecomp PRIVATE /STACK:268435456 /SUBSYSTEM:CONSOLE /OPT:NOICF)
endif()
if(MINGW)
    set_target_properties(SonicColorsRecomp PROPERTIES WIN32_EXECUTABLE FALSE)
    target_compile_options(SonicColorsRecomp PRIVATE -include ${CMAKE_SOURCE_DIR}/runtime/platform/compat.h
                           -fno-strict-aliasing -Wno-unused-function -Wno-unused-variable -Wa,-mbig-obj)
    # Large guest fiber stacks; static libgcc/libstdc++ so the zip needs no MinGW DLLs.
    # -mconsole: force console subsystem (avoid WinMain).
    # Static libgcc/libstdc++. winpthread may still be dynamic; package_windows_cross.sh ships it.
    target_link_options(SonicColorsRecomp PRIVATE -mconsole -Wl,--stack,268435456
                        -static-libgcc -static-libstdc++)
    # Prefer static winpthread when the archive is present (avoids shipping libwinpthread-1.dll).
    if(EXISTS /usr/x86_64-w64-mingw32/lib/libwinpthread.a)
        target_link_libraries(SonicColorsRecomp PRIVATE /usr/x86_64-w64-mingw32/lib/libwinpthread.a)
    endif()
    if(SCR_NOD_DLL)
        add_custom_command(TARGET SonicColorsRecomp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SCR_NOD_DLL}" $<TARGET_FILE_DIR:SonicColorsRecomp>
            VERBATIM)
    endif()
    if(EXISTS /usr/x86_64-w64-mingw32/bin/libwinpthread-1.dll)
        add_custom_command(TARGET SonicColorsRecomp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    /usr/x86_64-w64-mingw32/bin/libwinpthread-1.dll $<TARGET_FILE_DIR:SonicColorsRecomp>
            VERBATIM)
    endif()
endif()
if(LINUX)
    # MSVC runtime names (compat.h) everywhere; type punning as with MSVC
    target_compile_options(SonicColorsRecomp PRIVATE -include ${CMAKE_SOURCE_DIR}/runtime/platform/compat.h
                           -fno-strict-aliasing -Wno-unused-function -Wno-unused-variable)
    # the C++ runtime inside the exe: it runs on distributions with an older libstdc++
    target_link_options(SonicColorsRecomp PRIVATE -static-libstdc++ -static-libgcc)
    # finds libSDL3 and dxvk next to itself (out/ and the AppImage)
    set_target_properties(SonicColorsRecomp PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
    add_custom_command(TARGET SonicColorsRecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_SONAME_FILE:SDL3-shared> ${DXVK_LIBS} $<TARGET_FILE_DIR:SonicColorsRecomp>
        VERBATIM)
endif()
