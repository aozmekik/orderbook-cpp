# A downstream project that consumes orderbook the two documented ways:
#
#   find_package, after `cmake --install`:
#     cmake -S tests/package -B build/package -DCMAKE_PREFIX_PATH=<prefix>
#
#   FetchContent, straight from a Git repository:
#     cmake -S tests/package -B build/package -DOB_FETCH_REPOSITORY=<url or path> \
#           -DOB_FETCH_TAG=<branch, tag or commit>
#
# -DCONSUMER_CXX_STANDARD=23 compiles the consumer as C++23, whatever standard
# the library was built with: the library's interface must not change with
# the language mode of the code that includes it.
#
# Then `cmake --build build/package && ./build/package/consumer`.
cmake_minimum_required(VERSION 3.25)
project(orderbook_consumer LANGUAGES CXX)

set(OB_FETCH_REPOSITORY "" CACHE STRING "Fetch orderbook from this Git repository instead of find_package")
set(OB_FETCH_TAG "main" CACHE STRING "Branch, tag or commit to fetch")
set(CONSUMER_CXX_STANDARD 20 CACHE STRING "C++ standard of the consumer (20 or 23)")

# A consumer module path with a generic module name in it, which a fetched
# orderbook must not resolve its own modules against.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

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
set_target_properties(consumer PROPERTIES CXX_STANDARD ${CONSUMER_CXX_STANDARD}
                                          CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
