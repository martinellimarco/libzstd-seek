cmake_minimum_required(VERSION 3.16)
project(libzstd-seek VERSION 1.3.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)

# --- Optional: sanitizers and coverage (mutually exclusive) ---
option(SANITIZE "Enable AddressSanitizer + UndefinedBehaviorSanitizer"  OFF)
option(COVERAGE "Enable LLVM coverage instrumentation"                  OFF)
option(FUZZ      "Build libFuzzer harnesses (requires Clang)"            OFF)
option(DEBUG_LOG "Enable debug messages on stderr (ZSTD_SEEK_DEBUG)"    OFF)

if(SANITIZE AND COVERAGE)
    message(FATAL_ERROR "SANITIZE and COVERAGE are mutually exclusive. Use separate build directories.")
endif()
if(FUZZ AND SANITIZE)
    message(FATAL_ERROR "FUZZ and SANITIZE are mutually exclusive (FUZZ includes ASAN+UBSAN)")
endif()
if(FUZZ AND COVERAGE)
    message(FATAL_ERROR "FUZZ and COVERAGE are mutually exclusive")
endif()

if(SANITIZE)
    message(STATUS "libzstd-seek: building with ASAN + UBSAN")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=address,undefined)
endif()

if(COVERAGE)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "COVERAGE=ON requires Clang (-fprofile-instr-generate is clang-only)")
    endif()
    message(STATUS "libzstd-seek: building with LLVM coverage instrumentation")
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping -g -O0)
    add_link_options(-fprofile-instr-generate)
endif()

# --- Locate zstd ---
# On Apple Silicon prefer /opt/homebrew to avoid stale x86_64 libs in /usr/local,
# but fall back to default paths for Intel Macs, MacPorts, and other setups.
if(APPLE)
    find_library(ZSTD_LIB NAMES zstd HINTS /opt/homebrew/lib NO_DEFAULT_PATH)
    find_path(ZSTD_INC NAMES zstd.h HINTS /opt/homebrew/include NO_DEFAULT_PATH)
    if(NOT ZSTD_LIB OR NOT ZSTD_INC)
        find_library(ZSTD_LIB NAMES zstd)
        find_path(ZSTD_INC NAMES zstd.h)
    endif()
else()
    find_library(ZSTD_LIB NAMES zstd)
    find_path(ZSTD_INC NAMES zstd.h)
endif()

if(NOT ZSTD_LIB OR NOT ZSTD_INC)
    message(FATAL_ERROR
        "zstd library or headers not found.\n"
        "  macOS:         brew install zstd\n"
        "  Debian/Ubuntu: apt install libzstd-dev\n"
        "  Arch:          pacman -S zstd\n")
endif()

# --- Library ---
add_library(zstd-seek zstd-seek.c zstd-seek.h mman_compat.h)
target_include_directories(zstd-seek PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${ZSTD_INC})
target_link_libraries(zstd-seek ${ZSTD_LIB} m)

if(DEBUG_LOG)
    target_compile_definitions(zstd-seek PRIVATE ZSTD_SEEK_DEBUG=1)
    message(STATUS "libzstd-seek: debug logging enabled (ZSTD_SEEK_DEBUG=1)")
endif()

option(BUILD_EXAMPLES "Build example programs" OFF)
if(BUILD_EXAMPLES AND NOT FUZZ)
    add_subdirectory(examples)
endif()

# --- Optional: test suite ---
option(BUILD_TESTS "Build the test suite" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# --- Optional: fuzz harnesses (libFuzzer) ---
if(FUZZ)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "FUZZ=ON requires Clang (libFuzzer is clang-only)")
    endif()
    message(STATUS "libzstd-seek: building fuzz harnesses with libFuzzer + ASAN + UBSAN")
    add_subdirectory(fuzz)
endif()
