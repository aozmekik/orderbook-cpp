cmake_minimum_required(VERSION 3.25)

project(
  orderbook
  VERSION 0.1.0
  DESCRIPTION "Limit order book and matching engine in C++20"
  HOMEPAGE_URL "https://github.com/aozmekik/orderbook-cpp"
  LANGUAGES CXX)

option(OB_BUILD_TESTS "Build the unit and differential tests" ${PROJECT_IS_TOP_LEVEL})
option(OB_BUILD_APPS "Build the obreplay and obgen command-line tools" ${PROJECT_IS_TOP_LEVEL})
option(OB_BUILD_EXAMPLES "Build the examples" ${PROJECT_IS_TOP_LEVEL})
option(OB_BUILD_BENCHMARKS "Build the Google Benchmark suite" ${PROJECT_IS_TOP_LEVEL})
option(OB_BUILD_FUZZERS "Build the libFuzzer targets (requires Clang with libFuzzer)" OFF)
option(OB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ${PROJECT_IS_TOP_LEVEL})
option(OB_INSTALL "Generate the install and package-config rules" ${PROJECT_IS_TOP_LEVEL})
option(OB_USE_STD_EXPECTED "Make ob::expected an alias of std::expected (requires C++23)" OFF)

if(PROJECT_IS_TOP_LEVEL)
  if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 20)
  endif()
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

include(GNUInstallDirs)
# Project modules are included by path, so that a module of the same name on a
# consumer's CMAKE_MODULE_PATH can never shadow them.
include(${PROJECT_SOURCE_DIR}/cmake/OrderbookWarnings.cmake)

# The project's own tests, tools and examples re-check every invariant of a
# book after each request in every build type except the optimised ones
# (Debug, or none). Consumers of the library opt in by defining
# OB_ENABLE_INVARIANT_CHECKS=1 themselves.
function(ob_enable_invariant_checks target)
  target_compile_definitions(
    ${target}
    PRIVATE $<$<NOT:$<CONFIG:Release,RelWithDebInfo,MinSizeRel>>:OB_ENABLE_INVARIANT_CHECKS=1>)
endfunction()

# --- Library -----------------------------------------------------------------

add_library(
  orderbook
  src/csv.cpp
  src/format.cpp
  src/hash_seed.cpp
  src/types.cpp
  src/workload.cpp)
add_library(orderbook::orderbook ALIAS orderbook)

# Settings that change types in the library's interface are recorded in a
# generated header, so that every translation unit agrees with the library.
if(OB_USE_STD_EXPECTED)
  set(OB_USE_STD_EXPECTED_VALUE 1)
else()
  set(OB_USE_STD_EXPECTED_VALUE 0)
endif()
configure_file(include/orderbook/config.hpp.in
               ${PROJECT_BINARY_DIR}/include/orderbook/config.hpp @ONLY)

target_include_directories(
  orderbook PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                   $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
                   $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
if(OB_USE_STD_EXPECTED)
  target_compile_features(orderbook PUBLIC cxx_std_23)
else()
  target_compile_features(orderbook PUBLIC cxx_std_20)
endif()
set_target_properties(orderbook PROPERTIES VERSION ${PROJECT_VERSION} EXPORT_NAME orderbook)
ob_enable_warnings(orderbook)

# --- Command-line tools, examples, tests, benchmarks, fuzzers ----------------

# Before any add_subdirectory: tests declared in a directory processed before
# enable_testing() (the examples, the benchmark smoke test) would be dropped.
if(OB_BUILD_TESTS)
  enable_testing()
endif()

if(OB_BUILD_APPS)
  add_subdirectory(apps)
endif()

if(OB_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(OB_BUILD_TESTS OR OB_BUILD_BENCHMARKS)
  include(${PROJECT_SOURCE_DIR}/cmake/OrderbookDependencies.cmake)
endif()

if(OB_BUILD_TESTS)
  add_subdirectory(tests)
endif()

if(OB_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()

if(OB_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()

# --- Install and package config ----------------------------------------------

if(OB_INSTALL)
  include(CMakePackageConfigHelpers)

  install(TARGETS orderbook EXPORT orderbookTargets)
  install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} PATTERN "*.in" EXCLUDE)
  install(FILES ${PROJECT_BINARY_DIR}/include/orderbook/config.hpp
          DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/orderbook)
  install(
    EXPORT orderbookTargets
    NAMESPACE orderbook::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/orderbook)

  configure_package_config_file(
    cmake/orderbookConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/orderbookConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/orderbook)
  write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/orderbookConfigVersion.cmake COMPATIBILITY SameMinorVersion)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/orderbookConfig.cmake
                ${CMAKE_CURRENT_BINARY_DIR}/orderbookConfigVersion.cmake
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/orderbook)
endif()
