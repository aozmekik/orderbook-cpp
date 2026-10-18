# A downstream project that consumes orderbook the two documented ways:
#
#   find_package, after `cmake --install`:
#     cmake -S tests/package -B build/package -DCMAKE_PREFIX_PATH=<prefix>
#
#   FetchContent, straight from a Git repository:
#     cmake -S tests/package -B build/package -DOB_FETCH_REPOSITORY=<url or path> \
#           -DOB_FETCH_TAG=<branch, tag or commit>
#
# Then `cmake --build build/package && ./build/package/consumer`.
cmake_minimum_required(VERSION 3.25)
project(orderbook_consumer LANGUAGES CXX)

set(OB_FETCH_REPOSITORY "" CACHE STRING "Fetch orderbook from this Git repository instead of find_package")
set(OB_FETCH_TAG "main" CACHE STRING "Branch, tag or commit to fetch")

if(OB_FETCH_REPOSITORY)
  include(FetchContent)
  FetchContent_Declare(
    orderbook
    GIT_REPOSITORY ${OB_FETCH_REPOSITORY}
    GIT_TAG ${OB_FETCH_TAG})
  FetchContent_MakeAvailable(orderbook)
else()
  find_package(orderbook 0.1 REQUIRED CONFIG)
endif()

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE orderbook::orderbook)
