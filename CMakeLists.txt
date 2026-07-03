cmake_minimum_required(VERSION 3.10)

# 项目只使用标准 C++ 和系统线程库，不依赖第三方库。
project(ThreadPool LANGUAGES CXX)

# 线程池内部使用 std::future、std::packaged_task 和 if constexpr，需要 C++17。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 在不同平台上正确找到 pthread、Windows 线程库等底层线程支持。
find_package(Threads REQUIRED)

# 示例程序：把线程池实现和 example/main 一起编译成可运行 demo。
add_executable(threadpool_demo
    example/example.cpp
    src/threadpool.cpp
)

# 暴露 public header 所在目录，让 example 可以 include "threadpool.h"。
target_include_directories(threadpool_demo PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

# 链接平台线程库；在 Linux/macOS 上通常会展开为 pthread 相关参数。
target_link_libraries(threadpool_demo PRIVATE Threads::Threads)
