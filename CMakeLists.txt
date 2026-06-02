cmake_minimum_required(VERSION 3.16)
project(cshift VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Options ───────────────────────────────────────────────────────────────────

option(CSHIFT_PRECOMPILE_FRT
    "Pre-compile frt.c for the host and every cross-target that has a toolchain"
    ON)

option(CSHIFT_BUNDLE_FRT
    "Copy precompiled frt.o blobs into the install tree under share/cshift/frt/"
    ON)

option(CSHIFT_LINK_STATIC
    "Link LLVM and C++ runtime statically (produces a more portable binary)"
    ON)

option(CSHIFT_BUNDLE_LIBCLANG
    "Copy libclang.so into the install/package tree (needed when CSHIFT_LINK_STATIC=ON)"
    ON)

# ── Find LLVM ─────────────────────────────────────────────────────────────────

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} in ${LLVM_DIR}")

# Statt einzelner Komponenten: nimm einfach alle, die das installierte LLVM hat.
if(CSHIFT_LINK_STATIC)
    llvm_map_components_to_libnames(CSHIFT_LLVM_LIBS all)
    message(STATUS "LLVM link mode: STATIC (all components)")
else()
    set(CSHIFT_LLVM_LIBS LLVM)
    message(STATUS "LLVM link mode: SHARED (libLLVM.so)")
endif()

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# ── Ubuntu versioned LLVM-C header path ───────────────────────────────────────

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

# ── Find libclang (optional — enables import <header.h>) ──────────────────────

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
    message(STATUS "Found libclang: ${LIBCLANG_LIB}")
    set(CSHIFT_HAVE_CHEADER ON)
else()
    message(STATUS "libclang not found — C-header import disabled at runtime")
    set(CSHIFT_HAVE_CHEADER OFF)
    set(CSHIFT_BUNDLE_LIBCLANG OFF)
endif()

# ── cshift executable ─────────────────────────────────────────────────────────

add_executable(cshift main.cpp)

target_compile_options(cshift PRIVATE
    -Wall -Wextra -O2
    -Wno-comment
)

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

# ── Linking ───────────────────────────────────────────────────────────────────

if(CSHIFT_LINK_STATIC)
    target_link_libraries(cshift PRIVATE ${CSHIFT_LLVM_LIBS})

    target_link_options(cshift PRIVATE
        -static-libstdc++
        -static-libgcc
    )

    find_library(TINFO_STATIC libtinfo.a
        HINTS /usr/lib/x86_64-linux-gnu /usr/lib)
    find_library(TINFO_SHARED tinfo
        HINTS /usr/lib/x86_64-linux-gnu /usr/lib)
    if(TINFO_STATIC)
        target_link_options(cshift PRIVATE
            "LINKER:-Bstatic" "-ltinfo" "LINKER:-Bdynamic")
        message(STATUS "tinfo: static (${TINFO_STATIC})")
    elseif(TINFO_SHARED)
        target_link_libraries(cshift PRIVATE ${TINFO_SHARED})
        message(STATUS "tinfo: shared (${TINFO_SHARED})")
    endif()

    if(CSHIFT_HAVE_CHEADER)
        target_link_libraries(cshift PRIVATE ${LIBCLANG_LIB})
        set_target_properties(cshift PROPERTIES
            INSTALL_RPATH "\$ORIGIN/../lib/cshift:\$ORIGIN/../lib:/usr/lib/x86_64-linux-gnu")
        set(CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)
    endif()

    target_link_libraries(cshift PRIVATE dl pthread)

else()
    target_link_libraries(cshift PRIVATE ${CSHIFT_LLVM_LIBS})
    if(CSHIFT_HAVE_CHEADER)
        target_link_libraries(cshift PRIVATE ${LIBCLANG_LIB})
    endif()
endif()

# ── Precompile frt.c ──────────────────────────────────────────────────────────

set(FRT_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/frt.c")

function(add_frt_target triple cc)
    find_program(_CC_${triple} "${cc}" QUIET)
    if(NOT _CC_${triple} AND NOT triple STREQUAL "native")
        message(STATUS "  frt: skipping ${triple} (${cc} not found)")
        return()
    endif()

    set(out_dir "${CMAKE_BINARY_DIR}/frt/${triple}")
    set(out_obj "${out_dir}/frt.o")
    file(MAKE_DIRECTORY "${out_dir}")

    if(triple STREQUAL "native")
        set(cc_bin "${CMAKE_C_COMPILER}")
    else()
        set(cc_bin "${_CC_${triple}}")
    endif()

    add_custom_command(
        OUTPUT  "${out_obj}"
        COMMAND "${cc_bin}" -O2 -c "${FRT_SOURCE}" -o "${out_obj}"
        DEPENDS "${FRT_SOURCE}"
        COMMENT "Compiling frt.c → frt/${triple}/frt.o  (${cc_bin})"
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
    add_frt_target("native"              "cc")
    add_frt_target("x86_64-linux-gnu"   "x86_64-linux-gnu-gcc")
    add_frt_target("aarch64-linux-gnu"  "aarch64-linux-gnu-gcc")
    add_frt_target("x86_64-w64-mingw32" "x86_64-w64-mingw32-gcc")

    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        add_frt_target("x86_64-apple-darwin"  "clang")
        add_frt_target("arm64-apple-darwin"   "clang")
    else()
        find_program(_OSXCROSS "x86_64-apple-darwin-clang" QUIET)
        if(_OSXCROSS)
            add_frt_target("x86_64-apple-darwin" "x86_64-apple-darwin-clang")
            add_frt_target("arm64-apple-darwin"  "arm64-apple-darwin-clang")
        endif()
    endif()
endif()

# ── Install rules ─────────────────────────────────────────────────────────────

include(GNUInstallDirs)

install(TARGETS cshift
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/std.cll"
    "${CMAKE_CURRENT_SOURCE_DIR}/frt.c"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/cshift"
)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/examples"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/cshift"
)

if(CSHIFT_HAVE_CHEADER AND CSHIFT_BUNDLE_LIBCLANG)
    get_filename_component(LIBCLANG_REAL "${LIBCLANG_LIB}" REALPATH)
    install(FILES "${LIBCLANG_REAL}"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cshift"
        RENAME "libclang.so.${LLVM_VERSION_MAJOR}"
    )
    install(CODE "
        execute_process(COMMAND \${CMAKE_COMMAND} -E create_symlink
            libclang.so.${LLVM_VERSION_MAJOR}
            \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/cshift/libclang.so\")
    ")
    message(STATUS "libclang bundle: ${LIBCLANG_REAL}")
endif()

# ── CPack etc. (falls du den Teil noch brauchst, kannst du ihn einfach wieder anhängen) ──

message(STATUS "")
message(STATUS "C<< (cshift) build summary")
message(STATUS "  Install prefix : ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  LLVM version   : ${LLVM_PACKAGE_VERSION}")
message(STATUS "  Link static    : ${CSHIFT_LINK_STATIC}")
message(STATUS "  C-header import: ${CSHIFT_HAVE_CHEADER}")
message(STATUS "  Bundle libclang: ${CSHIFT_BUNDLE_LIBCLANG}")
message(STATUS "  Precompile frt : ${CSHIFT_PRECOMPILE_FRT}")
message(STATUS "  Bundle frt     : ${CSHIFT_BUNDLE_FRT}")
message(STATUS "")
