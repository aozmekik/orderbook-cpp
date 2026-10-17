cmake_minimum_required(VERSION 3.25)

project(
  orderbook
  VERSION 0.1.0
  DESCRIPTION "Limit order book and matching engine in C++20"
  HOMEPAGE_URL "https://github.com/aozmekik/orderbook-cpp"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(OB_BUILD_APPS "Build the obreplay and obgen command-line tools" ${PROJECT_IS_TOP_LEVEL})
option(OB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ${PROJECT_IS_TOP_LEVEL})
option(OB_INSTALL "Generate the install and package-config rules" ${PROJECT_IS_TOP_LEVEL})

if(PROJECT_IS_TOP_LEVEL)
  if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 20)
  endif()
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

include(GNUInstallDirs)
include(Warnings)

# --- Library -----------------------------------------------------------------

add_library(
  orderbook
  src/csv.cpp
  src/format.cpp
  src/types.cpp
  src/workload.cpp)
add_library(orderbook::orderbook ALIAS orderbook)

target_include_directories(
  orderbook PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                   $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(orderbook PUBLIC cxx_std_20)
set_target_properties(orderbook PROPERTIES VERSION ${PROJECT_VERSION} EXPORT_NAME orderbook)
ob_enable_warnings(orderbook)

# --- Command-line tools ------------------------------------------------------

if(OB_BUILD_APPS)
  add_subdirectory(apps)
endif()

# --- Install and package config ----------------------------------------------

if(OB_INSTALL)
  include(CMakePackageConfigHelpers)

  install(TARGETS orderbook EXPORT orderbookTargets)
  install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
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
