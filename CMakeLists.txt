cmake_minimum_required(VERSION 3.15)

# dsidle: the accepted RelWithDebInfo flags use Clang's full LTO spelling.
# Select the locally installed LLVM toolchain before project() unless callers
# explicitly selected a compiler themselves.
if(NOT DEFINED CMAKE_C_COMPILER)
  find_program(DSIDLE_CLANG_C clang-18)
  if(DSIDLE_CLANG_C)
    set(CMAKE_C_COMPILER "${DSIDLE_CLANG_C}" CACHE FILEPATH "" FORCE)
  endif()
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER)
  find_program(DSIDLE_CLANG_CXX clang++-18)
  if(DSIDLE_CLANG_CXX)
    set(CMAKE_CXX_COMPILER "${DSIDLE_CLANG_CXX}" CACHE FILEPATH "" FORCE)
  endif()
endif()
project(D_SIDLE LANGUAGES C CXX)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR "D-SIDLE requires Clang for the mandated -flto=full acceptance build")
endif()

include(CTest)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# dsidle: RelWithDebInfo is the only acceptance build.  Keep these flags local
# to D-SIDLE rather than inheriting the old single-node DAX build options.
if(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
  add_compile_options(-O3 -g3 -march=native -flto=full)
  add_link_options(-flto=full)
endif()

add_library(dsidle STATIC dsidle/config.cc dsidle/shared_pool.cc dsidle/shard_allocator.cc dsidle/replica_directory.cc dsidle/latency_simulator.cc)
target_include_directories(dsidle PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(dsidle PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(dsidle PUBLIC atomic)

# dsidle: Build Masstree from this tree only. The legacy CXL allocator source
# is deliberately excluded; allocation conversion proceeds in the original
# Masstree files rather than through a compatibility backend.
add_library(dsidle_masstree STATIC
  third_party/masstree-beta/compiler.cc
  third_party/masstree-beta/json.cc
  third_party/masstree-beta/kvthread.cc
  third_party/masstree-beta/log.cc
  third_party/masstree-beta/memdebug.cc
  third_party/masstree-beta/msgpack.cc
  third_party/masstree-beta/query_masstree.cc
  third_party/masstree-beta/string.cc
  third_party/masstree-beta/string_slice.cc
  third_party/masstree-beta/str.cc
  third_party/masstree-beta/straccum.cc
  third_party/masstree-beta/value_array.cc
  third_party/masstree-beta/value_string.cc
  third_party/masstree-beta/value_versioned_array.cc
  third_party/sidle_utils/sidle_worker.cc)
target_include_directories(dsidle_masstree PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/masstree-beta
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sidle_utils
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/art
  ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(dsidle_masstree PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/third_party/masstree-beta/config.h -Wall -Wextra -Wpedantic)
target_link_libraries(dsidle_masstree PUBLIC dsidle pthread)

add_executable(dsidle_shared_pool dsidle/shared_pool_main.cc)
target_link_libraries(dsidle_shared_pool PRIVATE dsidle)

add_executable(dsidle_e2e_trace_runner dsidle/e2e_trace_runner.cc)
target_link_libraries(dsidle_e2e_trace_runner PRIVATE dsidle_masstree)
target_compile_options(dsidle_e2e_trace_runner PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/third_party/masstree-beta/config.h)

add_executable(dsidle_config_tests tests/config_tests.cc)
target_link_libraries(dsidle_config_tests PRIVATE dsidle)
target_compile_options(dsidle_config_tests PRIVATE -UNDEBUG)
add_test(NAME dsidle_config_tests COMMAND dsidle_config_tests)
set_tests_properties(dsidle_config_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(dsidle_shared_pool_tests tests/shared_pool_tests.cc)
target_link_libraries(dsidle_shared_pool_tests PRIVATE dsidle)
target_compile_options(dsidle_shared_pool_tests PRIVATE -UNDEBUG)
add_test(NAME dsidle_shared_pool_tests COMMAND dsidle_shared_pool_tests)

add_executable(dsidle_shard_allocator_tests tests/shard_allocator_tests.cc)
target_link_libraries(dsidle_shard_allocator_tests PRIVATE dsidle)
target_compile_options(dsidle_shard_allocator_tests PRIVATE -UNDEBUG)
add_test(NAME dsidle_shard_allocator_tests COMMAND dsidle_shard_allocator_tests)

add_executable(dsidle_epoch_tests tests/epoch_tests.cc)
target_link_libraries(dsidle_epoch_tests PRIVATE dsidle)
target_compile_options(dsidle_epoch_tests PRIVATE -UNDEBUG)
add_test(NAME dsidle_epoch_tests COMMAND dsidle_epoch_tests)

add_executable(dsidle_node_control_tests tests/node_control_tests.cc)
target_link_libraries(dsidle_node_control_tests PRIVATE dsidle)
target_compile_options(dsidle_node_control_tests PRIVATE -UNDEBUG)
add_test(NAME dsidle_node_control_tests COMMAND dsidle_node_control_tests)

add_executable(dsidle_replica_directory_tests tests/replica_directory_tests.cc)
target_link_libraries(dsidle_replica_directory_tests PRIVATE dsidle)
target_compile_options(dsidle_replica_directory_tests PRIVATE -UNDEBUG)
add_test(NAME dsidle_replica_directory_tests COMMAND dsidle_replica_directory_tests)

add_executable(dsidle_latency_simulator_tests tests/latency_simulator_tests.cc)
target_link_libraries(dsidle_latency_simulator_tests PRIVATE dsidle)
target_compile_options(dsidle_latency_simulator_tests PRIVATE -UNDEBUG)
add_test(NAME dsidle_latency_simulator_tests COMMAND dsidle_latency_simulator_tests)

add_executable(dsidle_masstree_single_process_tests tests/masstree_single_process_tests.cc)
target_link_libraries(dsidle_masstree_single_process_tests PRIVATE dsidle_masstree)
target_compile_options(dsidle_masstree_single_process_tests PRIVATE -UNDEBUG -include ${CMAKE_CURRENT_SOURCE_DIR}/third_party/masstree-beta/config.h)
add_test(NAME dsidle_masstree_single_process_tests COMMAND dsidle_masstree_single_process_tests)
