cmake_minimum_required(VERSION 3.16)
project(cshift CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ──────────────────────────────────────────────────────────────────────────────
# Force MSVC on Windows
# ──────────────────────────────────────────────────────────────────────────────

if(WIN32 AND NOT MSVC)
    message(FATAL_ERROR
        "On Windows, cshift must be built with MSVC (cl.exe). "
        "LLVM for Windows is built with MSVC and cannot be linked with MinGW. "
        "Use: cmake -G \"Visual Studio 17 2022\" -A x64"
    )
endif()

# ──────────────────────────────────────────────────────────────────────────────
# STATIC CRT (/MT)
# ──────────────────────────────────────────────────────────────────────────────

if(MSVC)
    foreach(flag_var
        CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE
        CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE)
        if(${flag_var} MATCHES "/MD")
            string(REGEX REPLACE "/MD" "/MT" ${flag_var} "${${flag_var}}")
        endif()
    endforeach()
endif()

# ──────────────────────────────────────────────────────────────────────────────
# Options
# ──────────────────────────────────────────────────────────────────────────────

option(CSHIFT_PRECOMPILE_FRT "Pre-compile frt.c" ON)
option(CSHIFT_BUNDLE_FRT "Bundle frt.o blobs" ON)

# ──────────────────────────────────────────────────────────────────────────────
# LLVM STATIC CONFIG
# ──────────────────────────────────────────────────────────────────────────────

set(LLVM_LINK_LLVM_DYLIB OFF)
set(LLVM_ENABLE_RTTI ON)
set(LLVM_ENABLE_EH ON)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} in ${LLVM_DIR}")

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
if(LLVM_LIBRARY_DIRS)
    link_directories(${LLVM_LIBRARY_DIRS})
endif()
add_definitions(${LLVM_DEFINITIONS})

# ──────────────────────────────────────────────────────────────────────────────
# libclang (optional)
# ──────────────────────────────────────────────────────────────────────────────

find_library(LIBCLANG_CPP NAMES clang-cpp HINTS ${LLVM_LIBRARY_DIRS})
find_library(LIBLLVM_C NAMES LLVM-C HINTS ${LLVM_LIBRARY_DIRS})

if(LIBCLANG_CPP)
    message(STATUS "Found clang-cpp: ${LIBCLANG_CPP}")
endif()

if(LIBLLVM_C)
    message(STATUS "Found LLVM-C: ${LIBLLVM_C}")
else()
    message(FATAL_ERROR "LLVM-C (C API) not found — required for your code")
endif()

# ──────────────────────────────────────────────────────────────────────────────
# Executable
# ──────────────────────────────────────────────────────────────────────────────

add_executable(cshift main.cpp)

if(MSVC)
    target_compile_options(cshift PRIVATE /W4 /O2)
else()
    target_compile_options(cshift PRIVATE -Wall -Wextra -O2)
endif()

# ──────────────────────────────────────────────────────────────────────────────
# STATIC LLVM COMPONENTS — ALL TARGETS
# ──────────────────────────────────────────────────────────────────────────────

llvm_map_components_to_libnames(LLVM_LIBS all)

target_link_libraries(cshift PRIVATE
    ${LLVM_LIBS}
    ${LIBLLVM_C}      # ← C API (fixes 90% of your undefined refs)
    ${LIBCLANG_CPP}   # ← optional, if present
)

# ──────────────────────────────────────────────────────────────────────────────
# Windows static system libs
# ──────────────────────────────────────────────────────────────────────────────

if(MSVC)
    target_link_libraries(cshift PRIVATE
        version
        shlwapi
        ws2_32
        bcrypt
    )
endif()

# ──────────────────────────────────────────────────────────────────────────────
# Precompile frt.c
# ──────────────────────────────────────────────────────────────────────────────

set(FRT_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/frt.c")

function(add_frt_target triple cc)
    find_program(CC_${triple} "${cc}" QUIET)

    if(NOT CC_${triple})
        message(STATUS "  frt: skipping ${triple} (${cc} not found)")
        return()
    endif()

    set(out_dir "${CMAKE_BINARY_DIR}/frt/${triple}")
    file(MAKE_DIRECTORY "${out_dir}")

    if(MSVC AND triple STREQUAL "native")
        set(out_obj "${out_dir}/frt.obj")
        set(cmd ${CC_${triple}} /O2 /c "${FRT_SOURCE}" /Fo"${out_obj}")
    else()
        set(out_obj "${out_dir}/frt.o")
        set(cmd ${CC_${triple}} -O2 -c "${FRT_SOURCE}" -o "${out_obj}")
    endif()

    add_custom_command(
        OUTPUT "${out_obj}"
        COMMAND ${cmd}
        DEPENDS "${FRT_SOURCE}"
        COMMENT "Compiling frt.c → frt/${triple}/frt.o (${CC_${triple}})"
        VERBATIM
    )

    add_custom_target("frt_${triple}" ALL DEPENDS "${out_obj}")
    add_dependencies(cshift "frt_${triple}")

    if(CSHIFT_BUNDLE_FRT)
        install(FILES "${out_obj}" DESTINATION "share/cshift/frt/${triple}")
    endif()
endfunction()

if(CSHIFT_PRECOMPILE_FRT)
    message(STATUS "Precompiling frt.c for available targets:")

    add_frt_target("native" "${CMAKE_C_COMPILER}")

    if(NOT WIN32)
        add_frt_target("x86_64-linux-gnu"   "x86_64-linux-gnu-gcc")
        add_frt_target("aarch64-linux-gnu"  "aarch64-linux-gnu-gcc")
        add_frt_target("x86_64-w64-mingw32" "x86_64-w64-mingw32-gcc")
    endif()

    if(APPLE)
        add_frt_target("x86_64-apple-darwin" "clang")
        add_frt_target("arm64-apple-darwin"  "clang")
    else()
        find_program(OSXCROSS_CLANG "x86_64-apple-darwin-clang" QUIET)
        if(OSXCROSS_CLANG)
            add_frt_target("x86_64-apple-darwin" "x86_64-apple-darwin-clang")
            add_frt_target("arm64-apple-darwin"  "arm64-apple-darwin-clang")
        endif()
    endif()
endif()

# ──────────────────────────────────────────────────────────────────────────────
# Install rules
# ──────────────────────────────────────────────────────────────────────────────

install(TARGETS cshift RUNTIME DESTINATION bin)

install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/std.cll"
    "${CMAKE_CURRENT_SOURCE_DIR}/frt.c"
    DESTINATION share/cshift
)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/examples"
    DESTINATION share/cshift
)

# ──────────────────────────────────────────────────────────────────────────────
# Summary
# ──────────────────────────────────────────────────────────────────────────────

message(STATUS "")
message(STATUS "C<< (cshift) build summary")
message(STATUS "  Install prefix : ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  LLVM version   : ${LLVM_PACKAGE_VERSION}")
message(STATUS "  Precompile frt : ${CSHIFT_PRECOMPILE_FRT}")
message(STATUS "  Bundle frt     : ${CSHIFT_BUNDLE_FRT}")
message(STATUS "")
