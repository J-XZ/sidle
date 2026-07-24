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

add_library(dsidle STATIC dsidle/config.cc dsidle/shared_pool.cc dsidle/shard_allocator.cc)
target_include_directories(dsidle PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(dsidle PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(dsidle PUBLIC atomic)

add_executable(dsidle_shared_pool dsidle/shared_pool_main.cc)
target_link_libraries(dsidle_shared_pool PRIVATE dsidle)

add_executable(dsidle_config_tests tests/config_tests.cc)
target_link_libraries(dsidle_config_tests PRIVATE dsidle)
add_test(NAME dsidle_config_tests COMMAND dsidle_config_tests)
set_tests_properties(dsidle_config_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(dsidle_shared_pool_tests tests/shared_pool_tests.cc)
target_link_libraries(dsidle_shared_pool_tests PRIVATE dsidle)
add_test(NAME dsidle_shared_pool_tests COMMAND dsidle_shared_pool_tests)

add_executable(dsidle_shard_allocator_tests tests/shard_allocator_tests.cc)
target_link_libraries(dsidle_shard_allocator_tests PRIVATE dsidle)
add_test(NAME dsidle_shard_allocator_tests COMMAND dsidle_shard_allocator_tests)

add_executable(dsidle_epoch_tests tests/epoch_tests.cc)
target_link_libraries(dsidle_epoch_tests PRIVATE dsidle)
add_test(NAME dsidle_epoch_tests COMMAND dsidle_epoch_tests)
