cmake_minimum_required(VERSION 3.10)

project(ThreadPool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_executable(threadpool_demo
    example/example.cpp
    src/threadpool.cpp
)

target_include_directories(threadpool_demo PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(threadpool_demo PRIVATE Threads::Threads)
