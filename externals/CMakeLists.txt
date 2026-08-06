# SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

set(BUILD_SHARED_LIBS OFF)
set(BUILD_TESTING OFF)
set_directory_properties(PROPERTIES
    EXCLUDE_FROM_ALL ON
    SYSTEM ON
)

# Set CMP0069 policy to "NEW" in order to ensure consistent behavior when building external targets with LTO enabled
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)

if (MSVC)
    # Silence "deprecation" warnings
    add_definitions(-D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE -D_SCL_SECURE_NO_WARNINGS)
endif()

# spdlog
set(SPDLOG_NO_EXCEPTIONS ON)
set(SPDLOG_DISABLE_GLOBAL_LOGGER ON)
if (TARGET fmt::fmt)
    set(SPDLOG_FMT_EXTERNAL ON)
endif()
add_subdirectory(spdlog EXCLUDE_FROM_ALL SYSTEM)
target_compile_definitions(spdlog PUBLIC SPDLOG_FUNCTION=__func__)

# CryptoPP
if (NOT TARGET cryptopp::cryptopp)
    set(CRYPTOPP_INSTALL OFF)
    set(CRYPTOPP_BUILD_TESTING OFF)
    set(CRYPTOPP_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/cryptopp)
    set(CRYPTOPP_DISABLE_AESNI ON)
    set(CRYPTOPP_DISABLE_AVX2 ON)
    add_subdirectory(cryptopp-cmake)
    if (WIN32)
        set(_windows_sdk_shared_dir "")
        set(_windows_kits_include_root "C:/Program Files (x86)/Windows Kits/10/Include")
        if (DEFINED CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION AND NOT CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION STREQUAL "")
            set(_windows_sdk_shared_candidate "${_windows_kits_include_root}/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/shared")
            if (EXISTS "${_windows_sdk_shared_candidate}/ksamd64.inc")
                set(_windows_sdk_shared_dir "${_windows_sdk_shared_candidate}")
            endif()
        endif()
        if (NOT _windows_sdk_shared_dir)
            file(GLOB _windows_sdk_shared_candidates LIST_DIRECTORIES true "${_windows_kits_include_root}/*/shared")
            list(REVERSE _windows_sdk_shared_candidates)
            foreach(_windows_sdk_shared_candidate IN LISTS _windows_sdk_shared_candidates)
                if (EXISTS "${_windows_sdk_shared_candidate}/ksamd64.inc")
                    set(_windows_sdk_shared_dir "${_windows_sdk_shared_candidate}")
                    break()
                endif()
            endforeach()
        endif()
        if (_windows_sdk_shared_dir)
            target_include_directories(cryptopp PRIVATE "${_windows_sdk_shared_dir}")
        endif()
        unset(_windows_sdk_shared_candidate)
        unset(_windows_sdk_shared_candidates)
        unset(_windows_sdk_shared_dir)
        unset(_windows_kits_include_root)
    endif()
    file(COPY cryptopp DESTINATION cryptopp FILES_MATCHING PATTERN "*.h")
    set_target_properties(cryptopp PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}/cryptopp")
endif()

# fmtlib
if (NOT TARGET fmt::fmt)
    add_subdirectory(fmt)
endif()

# SDL3
if (NOT TARGET SDL3::SDL3)
    set(SDL_TEST_LIBRARY OFF)
    set(SDL_PIPEWIRE OFF)
    add_subdirectory(sdl3)
endif()

# Toml11
if (NOT TARGET toml11::toml11)
    add_subdirectory(toml11)

    if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
      if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        get_target_property(_toml11_compile_options toml11 INTERFACE_COMPILE_OPTIONS)
        list(REMOVE_ITEM _toml11_compile_options "/Zc:preprocessor")
        set_target_properties(toml11 PROPERTIES INTERFACE_COMPILE_OPTIONS ${_toml11_compile_options})
      endif()
    endif()
endif()

#openal
if (NOT TARGET OpenAL::OpenAL)
set(ALSOFT_ENABLE_MODULES OFF CACHE BOOL "" FORCE)
set(LIBTYPE "STATIC" CACHE STRING "" FORCE)
# Disable everything we don't need
set(ALSOFT_UTILS OFF CACHE BOOL "" FORCE)
set(ALSOFT_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ALSOFT_TESTS OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL OFF CACHE BOOL "" FORCE)
set(ALSOFT_CONFIG OFF CACHE BOOL "" FORCE)

# Backends (platform-specific)
if (WIN32)
    set(ALSOFT_BACKEND_WASAPI ON CACHE BOOL "" FORCE)
    set(ALSOFT_BACKEND_DSOUND OFF CACHE BOOL "" FORCE)
    set(ALSOFT_BACKEND_WINMM OFF CACHE BOOL "" FORCE)
elseif (APPLE)
    set(ALSOFT_BACKEND_COREAUDIO ON CACHE BOOL "" FORCE)
else()
    set(ALSOFT_BACKEND_ALSA ON CACHE BOOL "" FORCE)
    set(ALSOFT_BACKEND_PULSEAUDIO ON CACHE BOOL "" FORCE)
    set(ALSOFT_BACKEND_PIPEWIRE OFF CACHE BOOL "" FORCE)
endif()
# Headless-safe
set(ALSOFT_BACKEND_NULL ON CACHE BOOL "" FORCE)

# Static build
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

add_subdirectory(openal-soft EXCLUDE_FROM_ALL)
endif()

#nlohmann json
set(JSON_BuildTests OFF CACHE INTERNAL "")
add_subdirectory(json)

# zlib (PFSC decompression)
if (NOT TARGET ZLIB::ZLIB)
    set(ZLIB_ENABLE_TESTS OFF)
    set(WITH_GTEST OFF)
    set(WITH_NEW_STRATEGIES ON)
    set(WITH_NATIVE_INSTRUCTIONS ON)
    set(ZLIB_COMPAT ON CACHE BOOL "" FORCE)
    include(FetchContent)
    FetchContent_Declare(
        ZLIB
        SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/zlib-ng"
        OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(ZLIB)
    add_library(ZLIB::ZLIB ALIAS zlib)
    set(ZLIB_INCLUDE_DIRS "${FETCHCONTENT_BASE_DIR}/zlib-build")
endif()

if (NOT APPLE)
    # vulkan-headers
    if (NOT TARGET Vulkan::Headers)
        set(VULKAN_HEADERS_ENABLE_MODULE OFF)
        add_subdirectory(vulkan-headers)
    endif()

    #volk
    add_subdirectory(volk)
endif()

# zstd (dependency of ZArchive)
if (NOT TARGET libzstd_static)
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
    set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
    add_subdirectory(zstd/build/cmake)
endif()

# ZArchive (.zar archive support)
if (NOT TARGET zarchive)
    add_library(zarchive STATIC
        zarchive/src/zarchivereader.cpp
        zarchive/src/zarchivewriter.cpp
        zarchive/src/sha_256.c
    )
    target_include_directories(zarchive PUBLIC zarchive/include)
    target_include_directories(zarchive PRIVATE zstd/lib)
    target_link_libraries(zarchive PRIVATE libzstd_static)
    target_compile_features(zarchive PRIVATE cxx_std_20)
    add_library(ZArchive::zarchive ALIAS zarchive)
endif()
