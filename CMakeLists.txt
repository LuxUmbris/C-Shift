cmake_minimum_required(VERSION 3.16)
project(cshift VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Options ───────────────────────────────────────────────────────────────────

option(CSHIFT_PRECOMPILE_FRT "Precompile frt.c" ON)
option(CSHIFT_BUNDLE_FRT "Bundle frt.o blobs" ON)
option(CSHIFT_LINK_STATIC "Static link LLVM" OFF)
option(CSHIFT_BUNDLE_LIBCLANG "Bundle libclang" ON)

# ── Find LLVM ─────────────────────────────────────────────────────────────────

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} in ${LLVM_DIR}")

# LLVM_DEFINITIONS fix for Windows/MSYS2
separate_arguments(LLVM_DEFINITIONS)
add_compile_options(${LLVM_DEFINITIONS})

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})

# Always dynamic linking → use monolithic LLVM
set(CSHIFT_LLVM_LIBS LLVM)

# ── LLVM-C headers (Ubuntu versioned) ─────────────────────────────────────────

set(CSHIFT_LLVMC_INCLUDE "")
foreach(_hint
    "/usr/include/llvm-c-${LLVM_VERSION_MAJOR}"
    "/usr/include/llvm-c-18"
    "/usr/include/llvm-c-17"
    "/usr/include/llvm-c-16"
    "${LLVM_INCLUDE_DIR}/..")
  if(EXISTS "${_hint}/llvm-c/Core.h")
    set(CSHIFT_LLVMC_INCLUDE "${_hint}")
    message(STATUS "LLVM-C headers: ${_hint}")
    break()
  endif()
endforeach()

# ── libclang (optional) ───────────────────────────────────────────────────────

find_library(LIBCLANG_LIB
    NAMES clang-${LLVM_VERSION_MAJOR} clang libclang
    HINTS
        ${LLVM_LIBRARY_DIR}
        /usr/lib/llvm-${LLVM_VERSION_MAJOR}/lib
        /usr/lib/x86_64-linux-gnu
        /usr/lib
)

find_path(LIBCLANG_INCLUDE_DIR
    NAMES clang-c/Index.h
    HINTS
        ${LLVM_INCLUDE_DIR}
        /usr/lib/llvm-${LLVM_VERSION_MAJOR}/include
)

if(LIBCLANG_LIB AND LIBCLANG_INCLUDE_DIR)
    set(CSHIFT_HAVE_CHEADER ON)
else()
    set(CSHIFT_HAVE_CHEADER OFF)
    set(CSHIFT_BUNDLE_LIBCLANG OFF)
endif()

# ── Executable ────────────────────────────────────────────────────────────────

add_executable(cshift main.cpp)

target_compile_options(cshift PRIVATE -Wall -Wextra -O2 -Wno-comment)

if(CSHIFT_LLVMC_INCLUDE)
    target_include_directories(cshift SYSTEM PRIVATE "${CSHIFT_LLVMC_INCLUDE}")
endif()

if(CSHIFT_HAVE_CHEADER)
    target_include_directories(cshift SYSTEM PRIVATE
        ${LIBCLANG_INCLUDE_DIR}
        ${LIBCLANG_INCLUDE_DIR}/clang-c
    )
    target_compile_definitions(cshift PRIVATE CSHIFT_HAVE_CHEADER=1)
endif()

target_compile_definitions(cshift PRIVATE CSHIFT_HAVE_LLVM_BITCODE=1)

# ── Linking (always dynamic) ──────────────────────────────────────────────────

target_link_libraries(cshift PRIVATE ${CSHIFT_LLVM_LIBS})

if(CSHIFT_HAVE_CHEADER)
    target_link_libraries(cshift PRIVATE ${LIBCLANG_LIB})
endif()

target_link_libraries(cshift PRIVATE dl pthread)

# ── Precompile frt.c ──────────────────────────────────────────────────────────

set(FRT_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/frt.c")

function(add_frt_target triple cc)
    find_program(CC_${triple} "${cc}" QUIET)
    if(NOT CC_${triple})
        message(STATUS "Skipping ${triple} (${cc} not found)")
        return()
    endif()

    set(out_dir "${CMAKE_BINARY_DIR}/frt/${triple}")
    file(MAKE_DIRECTORY "${out_dir}")
    set(out_obj "${out_dir}/frt.o")

    add_custom_command(
        OUTPUT "${out_obj}"
        COMMAND ${CC_${triple}} -O2 -c "${FRT_SOURCE}" -o "${out_obj}"
        DEPENDS "${FRT_SOURCE}"
        COMMENT "Compiling frt.c → frt/${triple}/frt.o"
    )

    add_custom_target("frt_${triple}" ALL DEPENDS "${out_obj}")
    add_dependencies(cshift "frt_${triple}")

    if(CSHIFT_BUNDLE_FRT)
        install(FILES "${out_obj}" DESTINATION "share/cshift/frt/${triple}")
    endif()
endfunction()

if(CSHIFT_PRECOMPILE_FRT)
    add_frt_target("native" "cc")
    add_frt_target("x86_64-linux-gnu" "x86_64-linux-gnu-gcc")
    add_frt_target("aarch64-linux-gnu" "aarch64-linux-gnu-gcc")
    add_frt_target("x86_64-w64-mingw32" "x86_64-w64-mingw32-gcc")
endif()

# ── Install rules ─────────────────────────────────────────────────────────────

include(GNUInstallDirs)

install(TARGETS cshift RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/std.cll"
    "${CMAKE_CURRENT_SOURCE_DIR}/frt.c"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/cshift"
)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/examples"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/cshift"
)

# Bundle libclang if requested
if(CSHIFT_HAVE_CHEADER AND CSHIFT_BUNDLE_LIBCLANG)
    get_filename_component(LIBCLANG_REAL "${LIBCLANG_LIB}" REALPATH)
    install(FILES "${LIBCLANG_REAL}"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cshift"
        RENAME "libclang.so.${LLVM_VERSION_MAJOR}"
    )
endif()

# ── Summary ───────────────────────────────────────────────────────────────────

message(STATUS "")
message(STATUS "C<< (cshift) build summary")
message(STATUS "  LLVM version   : ${LLVM_PACKAGE_VERSION}")
message(STATUS "  Link static    : ${CSHIFT_LINK_STATIC}")
message(STATUS "  C-header import: ${CSHIFT_HAVE_CHEADER}")
message(STATUS "  Precompile frt : ${CSHIFT_PRECOMPILE_FRT}")
message(STATUS "")
