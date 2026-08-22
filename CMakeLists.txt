# pd-audiofile -- a sound-file library for Pure Data
# SPDX-License-Identifier: Zlib

cmake_minimum_required(VERSION 3.16)
project(pd-audiofile VERSION 0.1.0 LANGUAGES C)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# C11 for <stdatomic.h>, which is what the audio thread and the control thread
# share, and for the assertions that the atomics it uses are lock-free.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    # MSVC's C11 atomics need asking for by name; they arrived in Visual Studio
    # 2022 17.5.
    add_compile_options(/experimental:c11atomics)
endif()

include(FetchContent)

# The two inputs the build has to locate are pinned, so that a build from a
# clean checkout is the same build everywhere. Point the *_INCLUDE_DIR options
# at a copy you already have to skip the fetch.
set(PD_GIT_TAG        "0.55-2" CACHE STRING "Pure Data tag to fetch m_pd.h from")
set(MINIAUDIO_GIT_TAG "9634bedb5b5a2ca38c1ee7108a9358a4e233f14d"
    CACHE STRING "miniaudio commit to fetch (0.11.25)")

set(PD_INCLUDE_DIR        "" CACHE PATH   "Directory containing m_pd.h")
set(PD_LIBRARY            "" CACHE FILEPATH "pd.lib -- required on Windows only")
set(PD_EXECUTABLE         "" CACHE FILEPATH "pd, for the smoke test; found if not set")
set(MINIAUDIO_INCLUDE_DIR "" CACHE PATH   "Directory containing miniaudio.h")

option(AUDIOFILE_MINIAUDIO_IMPLEMENTATION
       "Compile miniaudio into this library. Turn OFF when the host application already does."
       ON)
option(AUDIOFILE_BUILD_TESTS "Build the core test binary" ON)
option(AUDIOFILE_HOST_STUB
       "Link a stub that compiles miniaudio itself, to exercise the OFF case"
       OFF)

# The MA_NO_* macros decide the layout of ma_engine_config, so a host that
# compiled miniaudio with them must pass the same set here.
set(AUDIOFILE_MINIAUDIO_DEFINES "" CACHE STRING
    "MA_NO_* macros the host compiled miniaudio with (semicolon separated)")

# ---------------------------------------------------------------------------
# Pure Data headers
# ---------------------------------------------------------------------------

if(PD_INCLUDE_DIR STREQUAL "")
    message(STATUS "PD_INCLUDE_DIR not set: fetching Pure Data ${PD_GIT_TAG}")
    # SOURCE_SUBDIR names a directory that does not exist, which is how you
    # fetch a repository without also building whatever its CMakeLists does.
    FetchContent_Declare(puredata
        GIT_REPOSITORY https://github.com/pure-data/pure-data.git
        GIT_TAG        ${PD_GIT_TAG}
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  headers-only)
    FetchContent_MakeAvailable(puredata)
    set(PD_INCLUDE_DIR "${puredata_SOURCE_DIR}/src")
endif()

if(NOT EXISTS "${PD_INCLUDE_DIR}/m_pd.h")
    message(FATAL_ERROR "m_pd.h not found in ${PD_INCLUDE_DIR}")
endif()
message(STATUS "Pd headers: ${PD_INCLUDE_DIR}")

# ---------------------------------------------------------------------------
# miniaudio
# ---------------------------------------------------------------------------

if(MINIAUDIO_INCLUDE_DIR STREQUAL "")
    message(STATUS "MINIAUDIO_INCLUDE_DIR not set: fetching miniaudio ${MINIAUDIO_GIT_TAG}")
    FetchContent_Declare(miniaudio
        GIT_REPOSITORY https://github.com/mackron/miniaudio.git
        GIT_TAG        ${MINIAUDIO_GIT_TAG}
        SOURCE_SUBDIR  headers-only)
    FetchContent_MakeAvailable(miniaudio)
    set(MINIAUDIO_INCLUDE_DIR "${miniaudio_SOURCE_DIR}")
endif()

if(NOT EXISTS "${MINIAUDIO_INCLUDE_DIR}/miniaudio.h")
    message(FATAL_ERROR "miniaudio.h not found in ${MINIAUDIO_INCLUDE_DIR}")
endif()
message(STATUS "miniaudio: ${MINIAUDIO_INCLUDE_DIR}")

# Vorbis lives in miniaudio's extras, so it needs the whole checkout.
set(AUDIOFILE_MINIAUDIO_EXTRAS "${MINIAUDIO_INCLUDE_DIR}/extras")
if(EXISTS "${AUDIOFILE_MINIAUDIO_EXTRAS}/stb_vorbis.c")
    set(AUDIOFILE_WITH_VORBIS 1)
    message(STATUS "Vorbis: enabled (stb_vorbis found)")
else()
    set(AUDIOFILE_WITH_VORBIS 0)
    message(STATUS "Vorbis: disabled (extras/stb_vorbis.c not found)")
endif()

# ---------------------------------------------------------------------------
# The core: no Pd, so the tests can have it too
# ---------------------------------------------------------------------------

set(AUDIOFILE_CORE_SOURCES src/audiofile_core.c)
if(AUDIOFILE_MINIAUDIO_IMPLEMENTATION)
    list(APPEND AUDIOFILE_CORE_SOURCES src/miniaudio_impl.c)
endif()
if(AUDIOFILE_HOST_STUB)
    if(AUDIOFILE_MINIAUDIO_IMPLEMENTATION)
        message(FATAL_ERROR
            "AUDIOFILE_HOST_STUB is for exercising AUDIOFILE_MINIAUDIO_IMPLEMENTATION=OFF")
    endif()
    list(APPEND AUDIOFILE_CORE_SOURCES tests/host_miniaudio.c)
endif()

add_library(audiofile_core STATIC ${AUDIOFILE_CORE_SOURCES})

target_include_directories(audiofile_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${MINIAUDIO_INCLUDE_DIR})
if(AUDIOFILE_WITH_VORBIS)
    target_include_directories(audiofile_core PRIVATE ${AUDIOFILE_MINIAUDIO_EXTRAS})
endif()

target_compile_definitions(audiofile_core PUBLIC
    AUDIOFILE_MINIAUDIO_IMPLEMENTATION=$<BOOL:${AUDIOFILE_MINIAUDIO_IMPLEMENTATION}>
    AUDIOFILE_WITH_VORBIS=${AUDIOFILE_WITH_VORBIS})

if(NOT AUDIOFILE_MINIAUDIO_IMPLEMENTATION)
    if(AUDIOFILE_HOST_STUB AND AUDIOFILE_MINIAUDIO_DEFINES STREQUAL "")
        # What tests/host_miniaudio.c compiles with.
        set(AUDIOFILE_MINIAUDIO_DEFINES MA_NO_DEVICE_IO MA_NO_GENERATION MA_NO_ENCODING)
    endif()
    target_compile_definitions(audiofile_core PUBLIC ${AUDIOFILE_MINIAUDIO_DEFINES})
endif()

if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(audiofile_core PUBLIC Threads::Threads)
endif()
if(UNIX AND NOT APPLE)
    target_link_libraries(audiofile_core PUBLIC m dl)
endif()

# miniaudio and stb_vorbis are other people's code; only ours is held to the
# warning level.
if(MSVC)
    target_compile_options(audiofile_core PRIVATE /W3)
else()
    target_compile_options(audiofile_core PRIVATE -Wall -Wextra)
    foreach(third_party src/miniaudio_impl.c tests/host_miniaudio.c)
        if("${third_party}" IN_LIST AUDIOFILE_CORE_SOURCES)
            set_source_files_properties(${third_party} PROPERTIES COMPILE_OPTIONS "-w")
        endif()
    endforeach()
endif()

# ---------------------------------------------------------------------------
# The external
# ---------------------------------------------------------------------------

if(APPLE)
    set(PD_EXTENSION ".pd_darwin")
elseif(WIN32)
    set(PD_EXTENSION ".dll")
else()
    set(PD_EXTENSION ".pd_linux")
endif()

add_library(audiofile MODULE
    src/af_info.c
    src/af_play_tilde.c
    src/audiofile_setup.c)

target_link_libraries(audiofile PRIVATE audiofile_core)
target_include_directories(audiofile PRIVATE ${PD_INCLUDE_DIR})
target_compile_definitions(audiofile PRIVATE
    AUDIOFILE_VERSION="${PROJECT_VERSION}")

set_target_properties(audiofile PROPERTIES
    PREFIX ""
    OUTPUT_NAME "audiofile"
    SUFFIX "${PD_EXTENSION}")

if(MSVC)
    target_compile_options(audiofile PRIVATE /W3)
else()
    # Pd's t_newmethod is void *(*)(void) and every constructor is cast to it.
    target_compile_options(audiofile PRIVATE -Wall -Wextra -Wno-cast-function-type)
endif()

if(APPLE)
    # Pd's symbols are resolved by the host at load time.
    target_link_options(audiofile PRIVATE
        "LINKER:-undefined" "LINKER:dynamic_lookup")
elseif(WIN32)
    if(PD_LIBRARY STREQUAL "")
        # A DLL cannot leave Pd's symbols undefined, so an import library is
        # not optional here the way it is elsewhere.
        find_library(PD_LIBRARY_FOUND NAMES pd
            HINTS "${PD_INCLUDE_DIR}/../bin" "${PD_INCLUDE_DIR}/../lib"
            NO_DEFAULT_PATH)
        if(PD_LIBRARY_FOUND)
            set(PD_LIBRARY "${PD_LIBRARY_FOUND}")
        else()
            message(FATAL_ERROR
                "On Windows the external must link against Pd's import library. "
                "Set -DPD_LIBRARY=<path to pd.lib>.")
        endif()
    endif()
    message(STATUS "Pd import library: ${PD_LIBRARY}")
    target_link_libraries(audiofile PRIVATE "${PD_LIBRARY}")
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(AUDIOFILE_BUILD_TESTS)
    enable_testing()

    find_package(Python3 COMPONENTS Interpreter)

    set(AUDIOFILE_FIXTURE_DIR "${CMAKE_CURRENT_BINARY_DIR}/fixtures")

    add_executable(test_core tests/test_core.c)
    target_link_libraries(test_core PRIVATE audiofile_core)
    target_include_directories(test_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    if(NOT MSVC)
        target_compile_options(test_core PRIVATE -Wall -Wextra)
        target_link_libraries(test_core PRIVATE m)
    endif()

    if(Python3_Interpreter_FOUND)
        add_custom_command(
            OUTPUT "${AUDIOFILE_FIXTURE_DIR}/sine_mono_44100_s16.wav"
            COMMAND ${Python3_EXECUTABLE}
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/make_fixtures.py"
                    "${AUDIOFILE_FIXTURE_DIR}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/make_fixtures.py"
            COMMENT "Generating test fixtures")
        add_custom_target(fixtures ALL
            DEPENDS "${AUDIOFILE_FIXTURE_DIR}/sine_mono_44100_s16.wav")
        add_dependencies(test_core fixtures)
    else()
        message(WARNING "Python 3 not found: fixtures will not be generated")
    endif()

    add_test(NAME core COMMAND test_core "${AUDIOFILE_FIXTURE_DIR}")

    # Drives the control thread and the audio thread at once. Meaningful under
    # ThreadSanitizer; on its own it checks only that the two coexist.
    add_executable(test_concurrency tests/test_concurrency.c)
    target_link_libraries(test_concurrency PRIVATE audiofile_core)
    target_include_directories(test_concurrency PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    if(NOT MSVC)
        target_compile_options(test_concurrency PRIVATE -Wall -Wextra)
    endif()
    if(Python3_Interpreter_FOUND)
        add_dependencies(test_concurrency fixtures)
    endif()
    add_test(NAME concurrency COMMAND test_concurrency "${AUDIOFILE_FIXTURE_DIR}")
    set_tests_properties(concurrency PROPERTIES SKIP_RETURN_CODE 77)

    # The script exits 77, a skip, when it cannot find a Pd to run.
    if(Python3_Interpreter_FOUND)
        # Pd's binary distribution puts pd next to the import library, which is
        # the only copy a Windows build is guaranteed to have.
        if(PD_EXECUTABLE STREQUAL "" AND NOT PD_LIBRARY STREQUAL "")
            get_filename_component(pd_bindir "${PD_LIBRARY}" DIRECTORY)
            find_program(PD_EXECUTABLE_FOUND NAMES pd
                         HINTS "${pd_bindir}" NO_DEFAULT_PATH)
            if(PD_EXECUTABLE_FOUND)
                set(PD_EXECUTABLE "${PD_EXECUTABLE_FOUND}")
            endif()
        endif()
        if(NOT PD_EXECUTABLE STREQUAL "")
            message(STATUS "Pd for the smoke test: ${PD_EXECUTABLE}")
        endif()

        add_test(NAME pd_smoke
                 COMMAND ${Python3_EXECUTABLE}
                         "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_pd_smoke.py"
                         "$<TARGET_FILE_DIR:audiofile>"
                         "${AUDIOFILE_FIXTURE_DIR}"
                         "${PD_EXECUTABLE}")
        set_tests_properties(pd_smoke PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

# ---------------------------------------------------------------------------
# Install: the layout Pd expects, a directory named after the library
# ---------------------------------------------------------------------------

install(TARGETS audiofile DESTINATION audiofile)
install(DIRECTORY help/ DESTINATION audiofile FILES_MATCHING PATTERN "*.pd")
# audiofile-meta.pd is what deken reads for the version and description.
install(FILES audiofile-meta.pd README.md LICENSE DESTINATION audiofile)
