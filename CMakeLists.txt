cmake_minimum_required(VERSION 3.16)
project(cshift CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Options ───────────────────────────────────────────────────────────────────

option(CSHIFT_PRECOMPILE_FRT
    "Pre-compile frt.c for the host and every cross-target that has a toolchain"
    ON)

option(CSHIFT_BUNDLE_FRT
    "Copy precompiled frt.o blobs into the install tree under share/cshift/frt/"
    ON)

# ── Find LLVM ─────────────────────────────────────────────────────────────────

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} in ${LLVM_DIR}")

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

llvm_map_components_to_libnames(LLVM_LIBS
    core analysis target
    x86     x86asmparser     x86codegen     x86desc     x86info
    aarch64 aarch64asmparser aarch64codegen aarch64desc aarch64info
    riscv   riscvasmparser   riscvcodegen   riscvdesc   riscvinfo
    arm     armasmparser     armcodegen     armdesc     arminfo
    all-targets
)

# ── cshift binary ─────────────────────────────────────────────────────────────

add_executable(cshift main.cpp)
target_compile_options(cshift PRIVATE -Wall -Wextra -O2)
target_link_libraries(cshift PRIVATE ${LLVM_LIBS})

# ── Precompile frt.c ──────────────────────────────────────────────────────────
#
# We compile frt.c once per target triple into:
#   ${CMAKE_BINARY_DIR}/frt/<triple>/frt.o
#
# The install step copies those blobs to:
#   share/cshift/frt/<triple>/frt.o
#
# Supported triples (requires the appropriate cross-compiler):
#   native             – always built if a C compiler is available
#   x86_64-linux-gnu
#   aarch64-linux-gnu
#   x86_64-apple-darwin   (macOS x86-64 — skip on non-Darwin unless cross-CC)
#   arm64-apple-darwin    (Apple Silicon)
#   x86_64-w64-mingw32    (Windows — requires mingw-w64)

set(FRT_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/frt.c")

# Helper to register one frt.o precompile target
# Usage: add_frt_target(<triple> <cc_binary>)
function(add_frt_target triple cc)
    # Detect whether this cross-compiler exists on the host
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

    # Install the blob
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

    # macOS targets — only try on Darwin or if cross-clang is present
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

install(TARGETS cshift RUNTIME DESTINATION bin)

install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/std.cll"
    "${CMAKE_CURRENT_SOURCE_DIR}/frt.c"
    DESTINATION share/cshift
)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/examples"
    DESTINATION share/cshift
)

# ── Summary ───────────────────────────────────────────────────────────────────

message(STATUS "")
message(STATUS "C<< (cshift) build summary")
message(STATUS "  Install prefix : ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  LLVM version   : ${LLVM_PACKAGE_VERSION}")
message(STATUS "  Precompile frt : ${CSHIFT_PRECOMPILE_FRT}")
message(STATUS "  Bundle frt     : ${CSHIFT_BUNDLE_FRT}")
message(STATUS "")
