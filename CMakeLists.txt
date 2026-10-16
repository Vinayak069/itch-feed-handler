cmake_minimum_required(VERSION 3.20)
project(itch_feed_handler CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra -Wpedantic)

option(SANITIZE "address/undefined or thread" "")
if(SANITIZE STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
elseif(SANITIZE STREQUAL "thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()

find_package(Threads REQUIRED)

add_library(itchfh
  src/messages.cpp
  src/framing.cpp
  src/order_book.cpp
  src/udp.cpp
  src/generator.cpp
  src/pipeline.cpp)
target_include_directories(itchfh PUBLIC include)
target_link_libraries(itchfh PUBLIC Threads::Threads)

foreach(tool itch_gen itch_replay feed_bench queue_bench)
  add_executable(${tool} tools/${tool}.cpp)
  target_link_libraries(${tool} PRIVATE itchfh)
endforeach()

enable_testing()
find_package(GTest REQUIRED)
add_executable(unit_tests
  tests/test_messages.cpp
  tests/test_framing.cpp
  tests/test_order_store.cpp
  tests/test_order_book.cpp
  tests/test_spsc.cpp
  tests/test_pipeline.cpp)
target_link_libraries(unit_tests PRIVATE itchfh GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(unit_tests DISCOVERY_TIMEOUT 60)
