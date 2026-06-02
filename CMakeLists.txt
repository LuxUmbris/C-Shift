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

option(CSHIFT_PRECOMPILE_FRT
    "Pre-compile frt.c for the host and every cross-target that has a toolchain"
    ON)

option(CSHIFT_BUNDLE_FRT
    "Copy precompiled frt.o blobs into the install tree under share/cshift/frt/"
    ON)

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

if(WIN32)
    find_library(LIBCLANG_LIB
        NAMES libclang clang
        HINTS ${LLVM_LIBRARY_DIRS}
    )
    find_path(LIBCLANG_INCLUDE_DIR
        NAMES clang-c/Index.h
        HINTS ${LLVM_INCLUDE_DIRS}
    )
else()
    find_library(LIBCLANG_LIB
        NAMES clang libclang
        HINTS ${LLVM_LIBRARY_DIR} /usr/lib/llvm-18/lib /usr/lib/x86_64-linux-gnu
    )
    find_path(LIBCLANG_INCLUDE_DIR
        NAMES clang-c/Index.h
        HINTS ${LLVM_INCLUDE_DIR} /usr/lib/llvm-18/include
    )
endif()

if(LIBCLANG_LIB AND LIBCLANG_INCLUDE_DIR)
    message(STATUS "Found libclang: ${LIBCLANG_LIB}")
    set(CSHIFT_HAVE_CHEADER ON)
else()
    message(STATUS "libclang not found — C-header import disabled")
    set(CSHIFT_HAVE_CHEADER OFF)
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
# STATIC LLVM COMPONENTS
# ──────────────────────────────────────────────────────────────────────────────

llvm_map_components_to_libnames(LLVM_LIBS
    core
    support
    irreader
    bitreader
    bitwriter
    mc
    mcparser
    option
    x86codegen
    x86asmparser
    x86desc
    x86info
)

target_link_libraries(cshift PRIVATE ${LLVM_LIBS})

# LLVM-C headers
foreach(_llvmc_hint
    "/usr/include/llvm-c-18"
    "/usr/include/llvm-c-17"
    "/usr/include/llvm-c-16"
    "${LLVM_INCLUDE_DIR}/.."
)
  if(EXISTS "${_llvmc_hint}/llvm-c/Core.h")
    target_include_directories(cshift SYSTEM PRIVATE "${_llvmc_hint}")
    message(STATUS "LLVM-C headers: ${_llvmc_hint}")
    break()
  endif()
endforeach()

# libclang static
if(CSHIFT_HAVE_CHEADER)
    target_include_directories(cshift SYSTEM PRIVATE
        ${LIBCLANG_INCLUDE_DIR}
        ${LIBCLANG_INCLUDE_DIR}/clang-c
    )
    target_compile_definitions(cshift PRIVATE CSHIFT_HAVE_CHEADER=1)
    target_link_libraries(cshift PRIVATE ${LIBCLANG_LIB})
endif()

target_compile_definitions(cshift PRIVATE CSHIFT_HAVE_LLVM_BITCODE=1)

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
        install(FILES "${out_obj}"
            DESTINATION "share/cshift/frt/${triple}"
        )
    endif()
endfunction()

if(CSHIFT_PRECOMPILE_FRT)
    message(STATUS "Precompiling frt.c for available targets:")

    add_frt_target("native" "${CMAKE_C_COMPILER}")

    if(NOT WIN32)
        add_frt_target("x86_64-linux-gnu"   "x86_64-linux-gnu-gcc")
        add_frt_target("aarch64-linux-gnu"  "aarch64-linux-gnu-gcc")
    endif()

    if(NOT WIN32)
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
message(STATUS "  C-header import: ${CSHIFT_HAVE_CHEADER} (libclang)")
message(STATUS "")
