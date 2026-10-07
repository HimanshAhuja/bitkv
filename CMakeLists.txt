cmake_minimum_required(VERSION 3.20)
project(bitkv LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# -DBITKV_SANITIZE=address  -> AddressSanitizer + UndefinedBehaviorSanitizer
# -DBITKV_SANITIZE=thread   -> ThreadSanitizer (data races)
set(BITKV_SANITIZE "" CACHE STRING "Sanitizer: address, thread, or empty")
if(BITKV_SANITIZE STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
elseif(BITKV_SANITIZE STREQUAL "thread")
  add_compile_options(-fsanitize=thread)
  add_link_options(-fsanitize=thread)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# ---- the engine ------------------------------------------------------------
add_library(bitkv
  src/crc32.cc
  src/record.cc
  src/file_util.cc
  src/db.cc)
target_include_directories(bitkv PUBLIC include PRIVATE src)
find_package(Threads REQUIRED)
target_link_libraries(bitkv PUBLIC Threads::Threads)

# ---- the Redis-protocol server (Linux: uses epoll) ------------------------
add_library(bitkv_server_lib server/resp.cc server/commands.cc server/metrics.cc)
target_link_libraries(bitkv_server_lib PUBLIC bitkv)
target_include_directories(bitkv_server_lib PUBLIC server)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(bitkv-server server/server_main.cc)
  target_link_libraries(bitkv-server PRIVATE bitkv_server_lib)
endif()

# ---- crash-injection harness and benchmarks -------------------------------
add_executable(crash_test tools/crash_test.cc)
target_link_libraries(crash_test PRIVATE bitkv)

add_executable(bench bench/bench.cc)
target_link_libraries(bench PRIVATE bitkv)

# ---- tests -----------------------------------------------------------------
option(BITKV_BUILD_TESTS "Build unit tests" ON)
if(BITKV_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(bitkv_tests
    tests/record_test.cc
    tests/db_test.cc
    tests/recovery_test.cc
    tests/merge_test.cc
    tests/concurrency_test.cc
    tests/model_test.cc
    tests/resp_test.cc)
  target_include_directories(bitkv_tests PRIVATE src)
  target_link_libraries(bitkv_tests PRIVATE bitkv bitkv_server_lib GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(bitkv_tests DISCOVERY_TIMEOUT 60)
endif()
