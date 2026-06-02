cmake_minimum_required(VERSION 3.16)
project(cshift VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(CSHIFT_PRECOMPILE_FRT "Precompile frt.c" ON)
option(CSHIFT_BUNDLE_FRT "Bundle frt.o blobs" ON)
option(CSHIFT_LINK_STATIC "Static link LLVM" ON)
option(CSHIFT_BUNDLE_LIBCLANG "Bundle libclang" ON)

# ──────────────────────────────────────────────────────────────────────────────
# LLVM
# ──────────────────────────────────────────────────────────────────────────────

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} in ${LLVM_DIR}")

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# ──────────────────────────────────────────────────────────────────────────────
# Backend-Komponenten unterscheiden sich zwischen macOS und Linux
# ──────────────────────────────────────────────────────────────────────────────

if(APPLE)
    message(STATUS "Using macOS LLVM backend component names")

    set(CSHIFT_LLVM_COMPONENTS
        core analysis scalaropts ipo transformutils passes
        bitreader bitwriter
        mc mcparser mcdisassembler
        x86 aarch64 arm riscv
        support
    )

else()
    message(STATUS "Using Linux LLVM 18 backend component names")

    set(CSHIFT_LLVM_COMPONENTS
        core analysis scalaropts ipo transformutils passes
        bitreader bitwriter
        mc mcparser mcdisassembler

        # X86
        x86codegen x86desc x86info x86asmparser x86asmprinter x86disassembler

        # AArch64
        aarch64codegen aarch64desc aarch64info
        aarch64asmparser aarch64asmprinter aarch64disassembler

        # ARM
        armcodegen armdesc arminfo
        armasmparser armasmprinter armdisassembler

        # RISCV
        riscvcodegen riscvdesc riscvinfo
        riscvassembler riscvdisassembler

        support
    )
endif()

if(CSHIFT_LINK_STATIC)
    llvm_map_components_to_libnames(CSHIFT_LLVM_LIBS ${CSHIFT_LLVM_COMPONENTS})
else()
    set(CSHIFT_LLVM_LIBS LLVM)
endif()

# ──────────────────────────────────────────────────────────────────────────────
# libclang optional
# ──────────────────────────────────────────────────────────────────────────────

find_library(LIBCLANG_LIB
    NAMES clang-${LLVM_VERSION_MAJOR} clang libclang
    HINTS ${LLVM_LIBRARY_DIR} /usr/lib /usr/lib/x86_64-linux-gnu
)

find_path(LIBCLANG_INCLUDE_DIR
    NAMES clang-c/Index.h
    HINTS ${LLVM_INCLUDE_DIR} /usr/lib/llvm-${LLVM_VERSION_MAJOR}/include
)

if(LIBCLANG_LIB AND LIBCLANG_INCLUDE_DIR)
    set(CSHIFT_HAVE_CHEADER ON)
else()
    set(CSHIFT_HAVE_CHEADER OFF)
    set(CSHIFT_BUNDLE_LIBCLANG OFF)
endif()

# ──────────────────────────────────────────────────────────────────────────────
# Executable
# ──────────────────────────────────────────────────────────────────────────────

add_executable(cshift main.cpp)

target_compile_options(cshift PRIVATE -Wall -Wextra -O2 -Wno-comment)

if(CSHIFT_HAVE_CHEADER)
    target_include_directories(cshift SYSTEM PRIVATE
        ${LIBCLANG_INCLUDE_DIR}
        ${LIBCLANG_INCLUDE_DIR}/clang-c
    )
    target_compile_definitions(cshift PRIVATE CSHIFT_HAVE_CHEADER=1)
endif()

target_compile_definitions(cshift PRIVATE CSHIFT_HAVE_LLVM_BITCODE=1)

# ──────────────────────────────────────────────────────────────────────────────
# Linking
# ──────────────────────────────────────────────────────────────────────────────

if(CSHIFT_LINK_STATIC)
    target_link_libraries(cshift PRIVATE ${CSHIFT_LLVM_LIBS})
    target_link_options(cshift PRIVATE -static-libstdc++ -static-libgcc)
    target_link_libraries(cshift PRIVATE dl pthread)

    if(CSHIFT_HAVE_CHEADER)
        target_link_libraries(cshift PRIVATE ${LIBCLANG_LIB})
    endif()
else()
    target_link_libraries(cshift PRIVATE ${CSHIFT_LLVM_LIBS})
    if(CSHIFT_HAVE_CHEADER)
        target_link_libraries(cshift PRIVATE ${LIBCLANG_LIB})
    endif()
endif()

# ──────────────────────────────────────────────────────────────────────────────
# frt.c precompile
# ──────────────────────────────────────────────────────────────────────────────

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

# ──────────────────────────────────────────────────────────────────────────────
# Install
# ──────────────────────────────────────────────────────────────────────────────

include(GNUInstallDirs)

install(TARGETS cshift RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES std.cll frt.c DESTINATION ${CMAKE_INSTALL_DATADIR}/cshift)
install(DIRECTORY examples DESTINATION ${CMAKE_INSTALL_DATADIR}/cshift)

# ──────────────────────────────────────────────────────────────────────────────
# Summary
# ──────────────────────────────────────────────────────────────────────────────

message(STATUS "")
message(STATUS "C<< (cshift) build summary")
message(STATUS "  LLVM version   : ${LLVM_PACKAGE_VERSION}")
message(STATUS "  Link static    : ${CSHIFT_LINK_STATIC}")
message(STATUS "  C-header import: ${CSHIFT_HAVE_CHEADER}")
message(STATUS "  Precompile frt : ${CSHIFT_PRECOMPILE_FRT}")
message(STATUS "")
