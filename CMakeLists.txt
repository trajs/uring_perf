cmake_minimum_required(VERSION 3.14)
project(uring_perf CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-O3 -march=native -Wall -Wextra -pthread)

include_directories(include third_party/liburing/src/include)

set(SOURCES
    src/main.cpp
    src/net_utils.cpp
    src/stats.cpp
    src/uring_engine.cpp
    src/worker.cpp
)

add_executable(uring_perf ${SOURCES})
target_link_libraries(uring_perf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/liburing/src/liburing.a pthread)
