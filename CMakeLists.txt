cmake_minimum_required(VERSION 3.16)
project(minikv LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)

add_library(minikv_core src/protocol.cpp)
target_include_directories(minikv_core PUBLIC include)

add_executable(minikv-server src/server.cpp)
target_link_libraries(minikv-server PRIVATE minikv_core)

add_executable(minikv-bench src/bench.cpp)
target_link_libraries(minikv-bench PRIVATE Threads::Threads)

enable_testing()
add_executable(test_protocol tests/test_protocol.cpp)
target_link_libraries(test_protocol PRIVATE minikv_core)
add_test(NAME protocol COMMAND test_protocol)
