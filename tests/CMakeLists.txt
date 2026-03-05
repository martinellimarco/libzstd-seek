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

# ── helper: register a reference comparison test (library vs zstd CLI) ───────
# add_reference_test(name SEED NUM_FRAMES FRAME_SIZE [GEN_FLAGS...])
function(add_reference_test name seed num_frames frame_size)
    add_test(
        NAME    ${name}
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_reference.sh
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

# ── helper: register a heavy (realistic) test ────────────────────────────────
# Requires t2sz in PATH; skipped automatically if unavailable.
# Use  ctest -LE heavy  to exclude these from a fast CI run.
function(add_heavy_test name test_id)
    add_test(
        NAME    ${name}
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_heavy.sh
                    $<TARGET_FILE:test_seek>
                    ${BLOBS_DIR}
                    ${test_id}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )
    set_tests_properties(${name} PROPERTIES
        TIMEOUT 300
        SKIP_RETURN_CODE 77
        LABELS "heavy"
    )
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
add_roundtrip_test(rt_100frames_no_content_size 11 100 1000 --no-content-size)

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
add_api_test(api_seek_cur_backward        seek_cur_backward)
add_api_test(api_seek_end                 seek_end)
add_api_test(api_seek_random              seek_random)
add_api_test(api_seek_out_of_file         seek_out_of_file)

# ── API: jump table ──────────────────────────────────────────────────────────
add_api_test(api_jt_auto                  jump_table_auto)
add_api_test(api_jt_progressive           jump_table_progressive)
add_api_test(api_jt_new_free              jump_table_new_free)
add_api_test(api_jt_manual                jump_table_manual)
add_api_test(api_jt_progressive_reads     jt_progressive_reads)

# ── API: seekable format ─────────────────────────────────────────────────────
add_api_test(api_seekable_basic           seekable_basic)
add_api_test(api_seekable_checksum        seekable_checksum)
add_api_test(api_seekable_vs_scan         seekable_vs_scan)
add_api_test(api_seekable_malformed       seekable_malformed_footer)

# ── API: info functions ──────────────────────────────────────────────────────
add_api_test(api_file_size                file_size)
add_api_test(api_last_known_size          last_known_size)
add_api_test(api_frame_count              frame_count)
add_api_test(api_is_multiframe            is_multiframe)
add_api_test(api_fileno                   fileno_check)
add_api_test(api_compressed_tell          compressed_tell)
add_api_test(api_compressed_tell_mono    compressed_tell_monotonic)
add_api_test(api_compressed_tell_seek    compressed_tell_seek)
add_api_test(api_compressed_tell_abs     compressed_tell_absolute)

# ── API: seek (large forward) ────────────────────────────────────────────────
add_api_test(api_seek_forward_large      seek_forward_large)

# ── API: read patterns ───────────────────────────────────────────────────────
add_api_test(api_read_byte_by_byte        read_byte_by_byte)
add_api_test(api_read_chunks              read_chunks)
add_api_test(api_frame_boundary           frame_boundary)
add_api_test(api_single_frame             single_frame)
add_api_test(api_large_read               large_read)
add_api_test(api_read_too_much            read_too_much)

# ── API: edge cases ─────────────────────────────────────────────────────────
add_api_test(api_seek_cur_zero           seek_cur_zero)
add_api_test(api_seek_to_same_pos        seek_to_same_pos)
add_api_test(api_fileno_buffer           fileno_buffer)
add_api_test(api_read_zero_bytes         read_zero_bytes)

# ── Error paths ──────────────────────────────────────────────────────────────
add_error_test(err_null                   error_null)
add_error_test(err_empty_file             error_empty_file)
add_error_test(err_truncated              error_truncated)
add_error_test(err_invalid_format         error_invalid_format)
add_error_test(err_seek_negative          error_seek_negative)
add_error_test(err_seek_beyond            error_seek_beyond)
add_error_test(err_seek_invalid_origin    error_seek_invalid_origin)
add_error_test(err_read_past_eof          error_read_past_eof)
add_error_test(err_corrupted_header       error_corrupted_header)
add_error_test(err_mixed_format           error_mixed_format)
add_error_test(err_corrupted_frame_data   error_corrupted_frame_data)
add_error_test(err_seektable_bad_offsets  error_seektable_bad_offsets)

# ── Heavy tests (realistic data via t2sz) ────────────────────────────────
add_heavy_test(heavy_text           heavy_text)
add_heavy_test(heavy_binary         heavy_binary)
add_heavy_test(heavy_zeros          heavy_zeros)
add_heavy_test(heavy_mixed          heavy_mixed)
add_heavy_test(heavy_level_max      heavy_level_max)
add_heavy_test(heavy_small_frames   heavy_small_frames)
add_heavy_test(heavy_single_frame   heavy_single_frame)

# ── Reference comparison tests (library vs zstd CLI) ────────────────────────
add_reference_test(ref_4frames_seekable              1   4     1024    --seekable)
add_reference_test(ref_4frames_no_seekable           2   4     1024)
add_reference_test(ref_100frames_seekable            3   100   1000    --seekable)
add_reference_test(ref_100frames_no_seekable         4   100   1000)
add_reference_test(ref_single_frame                  5   1     65536)
add_reference_test(ref_single_frame_seekable         6   1     65536   --seekable)
add_reference_test(ref_large_frames                  7   10    1048576)
add_reference_test(ref_no_content_size               8   4     1024    --no-content-size)
add_reference_test(ref_vary_size                     9   10    10000   --seekable  --vary-size)
add_reference_test(ref_seekable_checksum            10   4     1024    --seekable  --checksum)
add_reference_test(ref_100frames_no_content_size    11   100   1000    --no-content-size)
add_reference_test(ref_many_small_frames            12   500   64      --seekable)
add_reference_test(ref_many_small_no_seekable       13   500   64)

# ── Apply COVERAGE / SANITIZE env vars to all tests ──────────────────────────
foreach(tname
    rt_4frames_seekable rt_4frames_no_seekable
    rt_100frames_seekable rt_100frames_no_seekable
    rt_single_frame rt_single_frame_seekable
    rt_large_frames rt_no_content_size
    rt_vary_size rt_seekable_checksum
    rt_100frames_no_content_size
    api_create_file api_create_file_no_jt
    api_create_buffer api_create_buffer_no_jt
    api_create_fd api_create_fd_no_jt
    api_seek_set_seq api_seek_set_backward
    api_seek_cur_forward api_seek_cur_backward
    api_seek_end api_seek_random api_seek_out_of_file
    api_jt_auto api_jt_progressive api_jt_new_free
    api_jt_manual api_jt_progressive_reads
    api_seekable_basic api_seekable_checksum api_seekable_vs_scan
    api_seekable_malformed
    api_file_size api_last_known_size api_frame_count
    api_is_multiframe api_fileno api_compressed_tell
    api_compressed_tell_mono api_compressed_tell_seek
    api_compressed_tell_abs
    api_seek_forward_large
    api_read_byte_by_byte api_read_chunks
    api_frame_boundary api_single_frame api_large_read
    api_read_too_much
    api_seek_cur_zero api_seek_to_same_pos api_fileno_buffer
    api_read_zero_bytes
    err_null err_empty_file err_truncated err_invalid_format
    err_seek_negative err_seek_beyond
    err_seek_invalid_origin err_read_past_eof
    err_corrupted_header err_mixed_format
    err_corrupted_frame_data err_seektable_bad_offsets
    ref_4frames_seekable ref_4frames_no_seekable
    ref_100frames_seekable ref_100frames_no_seekable
    ref_single_frame ref_single_frame_seekable
    ref_large_frames ref_no_content_size
    ref_vary_size ref_seekable_checksum
    ref_100frames_no_content_size
    ref_many_small_frames ref_many_small_no_seekable
    heavy_text heavy_binary heavy_zeros heavy_mixed
    heavy_level_max heavy_small_frames heavy_single_frame)
    set_test_env(${tname})
endforeach()
