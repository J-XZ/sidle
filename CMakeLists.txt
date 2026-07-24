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

add_library(dsidle_config STATIC dsidle/config.cc)
target_include_directories(dsidle_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(dsidle_config PRIVATE -Wall -Wextra -Wpedantic)

add_executable(dsidle_config_tests tests/config_tests.cc)
target_link_libraries(dsidle_config_tests PRIVATE dsidle_config)
add_test(NAME dsidle_config_tests COMMAND dsidle_config_tests)
set_tests_properties(dsidle_config_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
