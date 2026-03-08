cmake_minimum_required(VERSION 3.16)

# ── Apple / Homebrew LLVM workarounds ─────────────────────────────────────────
#
# 1. libc++ mismatch: Homebrew LLVM's libFuzzer runtime is linked against its
#    own libc++, which contains symbols absent from Apple's system libc++. We
#    detect the compiler's lib/c++ directory and add it to the link path.
#
# 2. UBSAN object-file incompatibility: The full -fsanitize=undefined generates
#    relocation entries that Apple's system linker cannot parse. The vptr and
#    function checks (C++-only, irrelevant for this pure-C library) are excluded.
if(APPLE)
    get_filename_component(_llvm_bindir "${CMAKE_C_COMPILER}" DIRECTORY)
    get_filename_component(_llvm_root   "${_llvm_bindir}" DIRECTORY)
    set(_llvm_libcxx "${_llvm_root}/lib/c++")
    if(EXISTS "${_llvm_libcxx}")
        set(FUZZ_EXTRA_LINK_FLAGS
            -L${_llvm_libcxx}
            "-Wl,-rpath,${_llvm_libcxx}"
        )
    endif()
    set(FUZZ_SANITIZE_FLAGS -fsanitize=fuzzer,address,undefined -fno-sanitize=vptr,function)
else()
    set(FUZZ_SANITIZE_FLAGS -fsanitize=fuzzer,address,undefined)
endif()

# ── Helper: register a libFuzzer harness ─────────────────────────────────────
function(add_fuzz_target name)
    add_executable(${name} ${name}.c)
    target_compile_options(${name} PRIVATE
        ${FUZZ_SANITIZE_FLAGS}
        -fno-omit-frame-pointer -g -O1
    )
    target_link_options(${name} PRIVATE
        ${FUZZ_SANITIZE_FLAGS}
        ${FUZZ_EXTRA_LINK_FLAGS}
    )
    target_link_libraries(${name} zstd-seek ${ZSTD_LIB} ${LIBM})
    target_include_directories(${name} PRIVATE ${ZSTD_INC})
endfunction()

# ── Fuzz targets ─────────────────────────────────────────────────────────────
add_fuzz_target(fuzz_decompress)
add_fuzz_target(fuzz_seekable)
add_fuzz_target(fuzz_jumptable)

# ── Seed corpus generation ───────────────────────────────────────────────────
find_program(ZSTD_CLI zstd REQUIRED)

set(CORPUS_DECOMPRESS  ${CMAKE_CURRENT_BINARY_DIR}/corpus_decompress)
set(CORPUS_SEEKABLE    ${CMAKE_CURRENT_BINARY_DIR}/corpus_seekable)
set(CORPUS_JUMPTABLE   ${CMAKE_CURRENT_BINARY_DIR}/corpus_jumptable)

add_custom_target(fuzz_corpus
    COMMAND ${CMAKE_COMMAND} -E env "ZSTD=${ZSTD_CLI}"
            bash ${CMAKE_CURRENT_SOURCE_DIR}/gen_corpus.sh
            ${CORPUS_DECOMPRESS} ${CORPUS_SEEKABLE} ${CORPUS_JUMPTABLE}
    COMMENT "Generating fuzz seed corpus in ${CMAKE_CURRENT_BINARY_DIR}"
)

# ── Dictionary ───────────────────────────────────────────────────────────────
# Copy the dictionary so it's available next to built fuzzers
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/zstd_seek.dict
               ${CMAKE_CURRENT_BINARY_DIR}/zstd_seek.dict COPYONLY)
