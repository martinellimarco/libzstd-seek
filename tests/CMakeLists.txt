cmake_minimum_required(VERSION 3.16)

set(CMAKE_C_STANDARD 99)

# ── gen_seekable: deterministic multi-frame zstd generator ───────────────────
add_executable(gen_seekable gen_seekable.c)
target_include_directories(gen_seekable PRIVATE ${ZSTD_INC})
target_link_libraries(gen_seekable ${ZSTD_LIB} m)

# ── test_seek: C test driver with dispatch by name ───────────────────────────
add_executable(test_seek test_seek.c)
target_link_libraries(test_seek zstd-seek)
target_include_directories(test_seek PRIVATE ${ZSTD_INC})

# ── blobs directory (created at configure time) ──────────────────────────────
set(BLOBS_DIR ${CMAKE_CURRENT_BINARY_DIR}/blobs)
file(MAKE_DIRECTORY ${BLOBS_DIR})

# ── helper: register a round-trip test ───────────────────────────────────────
# add_roundtrip_test(name SEED NUM_FRAMES FRAME_SIZE [GEN_FLAGS...])
function(add_roundtrip_test name seed num_frames frame_size)
    add_test(
        NAME    ${name}
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_roundtrip.sh
                    $<TARGET_FILE:test_seek>
                    $<TARGET_FILE:gen_seekable>
                    ${BLOBS_DIR}
                    ${seed}
                    ${num_frames}
                    ${frame_size}
                    ${ARGN}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )
    set_tests_properties(${name} PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
endfunction()

# ── helper: register an API test ─────────────────────────────────────────────
function(add_api_test name test_id)
    add_test(
        NAME    ${name}
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_api.sh
                    $<TARGET_FILE:test_seek>
                    $<TARGET_FILE:gen_seekable>
                    ${BLOBS_DIR}
                    ${test_id}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )
    set_tests_properties(${name} PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
endfunction()

# ── helper: register an error-path test ──────────────────────────────────────
function(add_error_test name test_id)
    add_test(
        NAME    ${name}
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_error_paths.sh
                    $<TARGET_FILE:test_seek>
                    $<TARGET_FILE:gen_seekable>
                    ${BLOBS_DIR}
                    ${test_id}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )
    set_tests_properties(${name} PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
endfunction()

# ── LLVM_PROFILE_FILE for coverage builds ────────────────────────────────────
if(COVERAGE)
    set(PROF_ENV "LLVM_PROFILE_FILE=${CMAKE_BINARY_DIR}/cov_%p.profraw")
endif()

function(set_test_env name)
    if(COVERAGE)
        set_tests_properties(${name} PROPERTIES ENVIRONMENT "${PROF_ENV}")
    endif()
    if(SANITIZE)
        if(APPLE)
            set_tests_properties(${name} PROPERTIES
                ENVIRONMENT "ASAN_OPTIONS=halt_on_error=1:print_stats=1")
        else()
            set_tests_properties(${name} PROPERTIES
                ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:print_stats=1")
        endif()
    endif()
endfunction()

# ══════════════════════════════════════════════════════════════════════════════
# Test matrix
# ══════════════════════════════════════════════════════════════════════════════

# ── Round-trip tests ─────────────────────────────────────────────────────────
add_roundtrip_test(rt_4frames_seekable         1  4  1024  --seekable)
add_roundtrip_test(rt_4frames_no_seekable      2  4  1024)
add_roundtrip_test(rt_100frames_seekable       3  100  1000  --seekable)
add_roundtrip_test(rt_100frames_no_seekable    4  100  1000)
add_roundtrip_test(rt_single_frame             5  1  65536)
add_roundtrip_test(rt_single_frame_seekable    6  1  65536  --seekable)
add_roundtrip_test(rt_large_frames             7  10  1048576)
add_roundtrip_test(rt_no_content_size          8  4  1024  --no-content-size)
add_roundtrip_test(rt_vary_size                9  10  10000  --seekable  --vary-size)
add_roundtrip_test(rt_seekable_checksum       10  4  1024  --seekable  --checksum)

# ── API: context creation (6 variants) ───────────────────────────────────────
add_api_test(api_create_file              create_from_file)
add_api_test(api_create_file_no_jt        create_from_file_no_jt)
add_api_test(api_create_buffer            create_from_buffer)
add_api_test(api_create_buffer_no_jt      create_from_buffer_no_jt)
add_api_test(api_create_fd                create_from_fd)
add_api_test(api_create_fd_no_jt          create_from_fd_no_jt)

# ── API: seek operations ─────────────────────────────────────────────────────
add_api_test(api_seek_set_seq             seek_set_sequential)
add_api_test(api_seek_set_backward        seek_set_backward)
add_api_test(api_seek_cur_forward         seek_cur_forward)
add_api_test(api_seek_end                 seek_end)
add_api_test(api_seek_random              seek_random)

# ── API: jump table ──────────────────────────────────────────────────────────
add_api_test(api_jt_auto                  jump_table_auto)
add_api_test(api_jt_progressive           jump_table_progressive)
add_api_test(api_jt_new_free              jump_table_new_free)

# ── API: seekable format ─────────────────────────────────────────────────────
add_api_test(api_seekable_basic           seekable_basic)
add_api_test(api_seekable_checksum        seekable_checksum)
add_api_test(api_seekable_vs_scan         seekable_vs_scan)

# ── API: info functions ──────────────────────────────────────────────────────
add_api_test(api_file_size                file_size)
add_api_test(api_last_known_size          last_known_size)
add_api_test(api_frame_count              frame_count)
add_api_test(api_is_multiframe            is_multiframe)
add_api_test(api_fileno                   fileno_check)
add_api_test(api_compressed_tell          compressed_tell)

# ── API: read patterns ───────────────────────────────────────────────────────
add_api_test(api_read_byte_by_byte        read_byte_by_byte)
add_api_test(api_read_chunks              read_chunks)
add_api_test(api_frame_boundary           frame_boundary)
add_api_test(api_single_frame             single_frame)
add_api_test(api_large_read               large_read)

# ── Error paths ──────────────────────────────────────────────────────────────
add_error_test(err_null                   error_null)
add_error_test(err_empty_file             error_empty_file)
add_error_test(err_truncated              error_truncated)
add_error_test(err_invalid_format         error_invalid_format)
add_error_test(err_seek_negative          error_seek_negative)
add_error_test(err_seek_beyond            error_seek_beyond)
add_error_test(err_seek_invalid_origin    error_seek_invalid_origin)
add_error_test(err_read_past_eof          error_read_past_eof)

# ── Apply COVERAGE / SANITIZE env vars to all tests ──────────────────────────
foreach(tname
    rt_4frames_seekable rt_4frames_no_seekable
    rt_100frames_seekable rt_100frames_no_seekable
    rt_single_frame rt_single_frame_seekable
    rt_large_frames rt_no_content_size
    rt_vary_size rt_seekable_checksum
    api_create_file api_create_file_no_jt
    api_create_buffer api_create_buffer_no_jt
    api_create_fd api_create_fd_no_jt
    api_seek_set_seq api_seek_set_backward
    api_seek_cur_forward api_seek_end api_seek_random
    api_jt_auto api_jt_progressive api_jt_new_free
    api_seekable_basic api_seekable_checksum api_seekable_vs_scan
    api_file_size api_last_known_size api_frame_count
    api_is_multiframe api_fileno api_compressed_tell
    api_read_byte_by_byte api_read_chunks
    api_frame_boundary api_single_frame api_large_read
    err_null err_empty_file err_truncated err_invalid_format
    err_seek_negative err_seek_beyond
    err_seek_invalid_origin err_read_past_eof)
    set_test_env(${tname})
endforeach()
